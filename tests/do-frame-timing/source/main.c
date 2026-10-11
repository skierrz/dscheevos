/*
 * Times rc_client_do_frame on the DS/DSi ARM9 with the hardware timers.
 *
 * Loads a synthetic achievement set through a fake server, then runs
 * FRAMES frames. Before each frame it changes some "game RAM" bytes and
 * (in cold mode) flushes both caches, the way a running game would leave
 * them. Results go to the screen, the no$gba debug port (read by the
 * headless emulator runner) and, when an SD card is present, to
 * /dscheevos_timing.txt.
 */
#include <nds.h>
#include <fat.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_client.h"
#include "rc_consoles.h"
#include "set.h"

#define FRAMES 300
#define BUS_HZ 33513982u               /* timer clock with TIMER_DIV_1 */
#define FRAME_US 16715u                /* 59.83 Hz */

/* ---- output: screen, debug port, optional SD log ---- */
static FILE* sdlog;
static void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char* fmt, ...)
{
	char line[160] = "@@";
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + 2, sizeof line - 3, fmt, ap);
	va_end(ap);
	strcat(line, "\n");
	nocashMessage(line);
	iprintf("%s", line + 2);
	if (sdlog) fputs(line + 2, sdlog);
}

/* ---- 32-bit tick counter from cascaded TIMER0/TIMER1 ---- */
static void timer_start(void)
{
	TIMER0_CR = 0; TIMER1_CR = 0;
	TIMER0_DATA = 0; TIMER1_DATA = 0;
	TIMER1_CR = TIMER_ENABLE | TIMER_CASCADE;
	TIMER0_CR = TIMER_ENABLE | TIMER_DIV_1;
}
static u32 ticks(void)
{
	u16 hi, lo, hi2;
	do { hi = TIMER1_DATA; lo = TIMER0_DATA; hi2 = TIMER1_DATA; } while (hi != hi2);
	return ((u32)hi << 16) | lo;
}
static u32 to_us(u32 t) { return (u32)(((u64)t * 1000000u) / BUS_HZ); }

/* ---- stand-in for the frozen game RAM snapshot ---- */
static u8* snap;            /* main RAM image (4 MB on DSi, 1 MB masked on DS) */
static u32 snap_mask;
static u8 snap_dtcm[0x4000];
static u32 rng = 12345;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint32_t read_mem(uint32_t a, uint8_t* b, uint32_t n, rc_client_t* c)
{
	(void)c;
	if (a < 0x400000 && a + n <= 0x400000) {
		u32 o = a & snap_mask;
		if (o + n > snap_mask + 1) o = snap_mask + 1 - n;
		memcpy(b, snap + o, n);
		return n;
	}
	if (a >= 0x1000000 && a + n <= 0x1004000) { memcpy(b, snap_dtcm + (a - 0x1000000), n); return n; }
	return 0;
}

/* ---- fake server: answers synchronously ---- */
static void server(const rc_api_request_t* r, rc_client_server_callback_t cb, void* d, rc_client_t* c)
{
	(void)c;
	rc_api_server_response_t resp;
	const char* body = "{\"Success\":true}";
	const char* p = r->post_data;
	if (strstr(p, "r=login2")) body = "{\"Success\":true,\"User\":\"User\",\"Token\":\"ApiToken\",\"Score\":1,\"Messages\":0}";
	else if (strstr(p, "r=achievementsets")) body = patch_json;
	else if (strstr(p, "r=startsession")) body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
	else if (strstr(p, "r=awardachievement")) body = "{\"Success\":true,\"Score\":1,\"SoftcoreScore\":0,\"AchievementID\":1,\"AchievementsRemaining\":1}";
	resp.body = body; resp.body_length = strlen(body); resp.http_status_code = 200;
	cb(&resp, d);
}

static int unlocks;
static void events(const rc_client_event_t* e, rc_client_t* c) { (void)c; if (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED) unlocks++; }
static void done(int r, const char* m, rc_client_t* c, void* u) { (void)c; (void)u; if (r) out("callback error %d: %s", r, m ? m : ""); }
static rc_clock_t now_ms;
static rc_clock_t clock_ms(const rc_client_t* c) { (void)c; return now_ms; }

static int cmp_u32(const void* a, const void* b) { u32 x = *(const u32*)a, y = *(const u32*)b; return x < y ? -1 : x > y; }

static void run(rc_client_t* client, int cold, const char* label)
{
	static u32 t[FRAMES];
	int i;
	u64 sum = 0;
	for (i = 0; i < FRAMES; i++) {
		int k;
		for (k = 0; k < 512; k++) snap[rnd() & snap_mask] = (u8)rnd();
		for (k = 0; k < 32; k++) snap_dtcm[rnd() & 0x3FFF] = (u8)rnd();
		now_ms += 17;
		if (cold) { DC_FlushAll(); IC_InvalidateAll(); }
		u32 t0 = ticks();
		rc_client_do_frame(client);
		t[i] = ticks() - t0;
		sum += t[i];
	}
	qsort(t, FRAMES, sizeof t[0], cmp_u32);
	u32 med = to_us(t[FRAMES / 2]), p95 = to_us(t[FRAMES * 95 / 100]), mx = to_us(t[FRAMES - 1]);
	out("RESULT %s %s min_us=%lu med_us=%lu p95_us=%lu max_us=%lu mean_us=%lu med_pct=%lu",
	    SET_NAME, label, (unsigned long)to_us(t[0]), (unsigned long)med, (unsigned long)p95,
	    (unsigned long)mx, (unsigned long)to_us((u32)(sum / FRAMES)), (unsigned long)(med * 100 / FRAME_US));
}

int main(void)
{
	consoleDemoInit();
	if (fatInitDefault()) sdlog = fopen("/dscheevos_timing.txt", "a");

	int dsi = isDSiMode();
#ifdef FAST_CPU
	if (dsi) setCpuClock(true);
#else
	if (dsi) setCpuClock(false);
#endif
	int mhz = (dsi && (REG_SCFG_CLK & 1)) ? 133 : 67;
	out("START set=%s build=%s console=%s cpu=%dMHz", SET_NAME, BUILD_NAME, dsi ? "dsi" : "ds", mhz);

	snap_mask = dsi ? 0x3FFFFF : 0xFFFFF;
	snap = malloc(snap_mask + 1);
	if (!snap) { out("FAIL no memory for snapshot"); goto end; }
	for (u32 i = 0; i <= snap_mask; i += 4) {
		u32 v = rnd();
		if ((v & 7) == 0) v = 0x02000000u | (rnd() & 0x3FFFFCu);  /* looks like a DS pointer */
		memcpy(snap + i, &v, 4);
	}

	timer_start();
	struct mallinfo m0 = mallinfo();

	rc_client_t* client = rc_client_create(read_mem, server);
	rc_client_set_event_handler(client, events);
	rc_client_set_get_time_millisecs_function(client, clock_ms);
	rc_client_set_hardcore_enabled(client, 1);
	rc_client_begin_login_with_token(client, "User", "ApiToken", done, NULL);

	u32 t0 = ticks();
	rc_client_begin_load_game(client, "0123456789ABCDEF0123456789ABCDEF", done, NULL);
	u32 load = ticks() - t0;
	struct mallinfo m1 = mallinfo();
	if (!rc_client_is_game_loaded(client)) { out("FAIL game did not load"); goto end; }
	out("LOAD %s load_ms=%lu heap_after_load=%d heap_arena=%d",
	    SET_NAME, (unsigned long)(to_us(load) / 1000), m1.uordblks - m0.uordblks, m1.arena);

	run(client, 0, "warm");
	run(client, 1, "cold");
	out("UNLOCKS %d", unlocks);

	rc_client_unload_game(client);
	rc_client_destroy(client);
end:
	out("DONE");
	if (sdlog) fclose(sdlog);
	while (1) swiWaitForVBlank();
}
