/*
 * Module test: loads dscheevos9.bin the way the nds-bootstrap hooks will,
 * drives it from a real VBlank IRQ and an idle loop, and checks unlocks,
 * the achievement list, notices and the offline journal in RAM.
 * The "game" is this program's own variables. Results go to the no$gba
 * debug port ("@@" lines) for the headless emulator runner.
 */
#include <nds.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "dscheevos_module.h"
#include "dscheevos9_bin.h"

static void out(const char* fmt, ...)
{
	char b[200] = "@@";
	va_list ap; va_start(ap, fmt); vsnprintf(b + 2, sizeof b - 3, fmt, ap); va_end(ap);
	strcat(b, "\n"); nocashMessage(b);
}

/* game state the test set watches */
static volatile u8 level;
static volatile u16 coins;
static volatile u32 object_ptr;      /* points at a struct, like a game's heap pointer */
static volatile u8 object[16];
static volatile u8 laps;

static const dscm_header_t* hdr;
static volatile u32 frame;

static void vblank_irq(void)
{
	frame++;
	/* as the ARM7 VBlank hook does: straight to RAM; the module reads it past the cache */
	((volatile dscm_header_t*)DSCM_UNCACHED(hdr))->vblank_count++;
	((void (*)(void))hdr->vblank)();
}

static u32 ra(volatile const void* p) { return (u32)p - 0x02000000; }

static int answered_awards[16], award_count;
static int lb_scores[4], lb_count;

static void network_layer(void)
{
	for (int i = 0; i < DSCM_NET_SLOTS; i++) {
		dscm_net_slot_t* s = &hdr->net[i];
		if (s->state != DSCM_NET_REQUEST) continue;
		s->state = DSCM_NET_IN_FLIGHT;
		const char* a = strstr(s->post, "&a=");
		const char* h = strstr(s->post, "&h=");
		if (strncmp(s->post, "r=awardachievement", 18) == 0 && a && award_count < 16) {
			answered_awards[award_count++] = atoi(a + 3) * (h && h[3] == '1' ? 1 : -1);
			siprintf(s->reply, "{\"Success\":true,\"Score\":1,\"SoftcoreScore\":0,\"AchievementID\":%d,\"AchievementsRemaining\":1}", atoi(a + 3));
		} else if (strncmp(s->post, "r=submitlbentry", 15) == 0 && strstr(s->post, "&s=") && lb_count < 4) {
			int score = atoi(strstr(s->post, "&s=") + 3);
			lb_scores[lb_count++] = score;
			siprintf(s->reply, "{\"Success\":true,\"Response\":{\"Score\":%d,\"BestScore\":%d,\"RankInfo\":{\"Rank\":1,\"NumEntries\":\"1\"},\"TopEntries\":[]}}", score, score);
		} else {
			strcpy(s->reply, "{\"Success\":true}");
		}
		s->reply_length = strlen(s->reply);
		s->http_status = 200;
		s->state = DSCM_NET_REPLIED;
	}
}

static int check(int ok, const char* what)
{
	out("%s: %s", ok ? "ok" : "FAIL", what);
	return !ok;
}

static const dscm_notice_t* find_notice(const dscm_notices_t* n, int kind, u32 id)
{
	for (u32 seq = n->head; seq > 0 && seq + DSCM_NOTICE_RING > n->head; seq--) {
		const dscm_notice_t* x = &n->ring[seq % DSCM_NOTICE_RING];
		if (x->seq == seq && x->kind == kind && (!id || x->id == id)) return x;
	}
	return NULL;
}

/* ABI v6: list, notices and journal, read as the in-game menu and the ARM7 would */
static int check_v6(void)
{
	int fails = 0;
	char b[120];

	/* list: header->list is written past the cache, so read it from RAM */
	const dscm_list_t* l = *(dscm_list_t* volatile*)DSCM_UNCACHED(&hdr->list);
	int unlocked = 0, order = 1;
	if (l && l->magic == DSCM_LIST_MAGIC && l->entry_size == sizeof(dscm_list_entry_t)) {
		for (u32 i = 0; i < l->count; i++) {
			unlocked += l->e[i].state == DSCM_ACH_UNLOCKED && l->e[i].unlock_time >= 1700000000;
			if (l->e[i].id != i + 1) order = 0;
		}
		siprintf(b, "list: %lu achievements, %d unlocked, generation %lu", (unsigned long)l->count, unlocked, (unsigned long)l->generation);
		fails += check(l->count == 5 && unlocked == 4 && order && l->e[3].state == DSCM_ACH_LOCKED && l->generation >= 2, b);
		fails += check(!strcmp(l->e[0].title, "Level 3") && !strcmp(l->e[0].description, "Reach level 3") && l->e[4].points == 10, "list titles");
	} else {
		fails += check(0, "list published");
	}

	/* notices */
	const dscm_notices_t* n = hdr->notices;
	const dscm_notice_t* x;
	fails += check(n->magic == DSCM_NOTICES_MAGIC, "notices magic");
	x = find_notice(n, DSCM_NOTICE_SESSION_START, 0);
	fails += check(x && x->seq == 1 && x->value == 1 && !strcmp(x->text, "Module Test") && !strcmp(x->detail, "0/5 achievements"), "notice: session start, hardcore");
	int unlock_notices = 0;
	for (u32 id = 1; id <= 5; id++) unlock_notices += find_notice(n, DSCM_NOTICE_UNLOCK, id) != NULL;
	siprintf(b, "notices: head %lu, %d unlocks", (unsigned long)n->head, unlock_notices);
	fails += check(unlock_notices == 4, b);
	fails += check(find_notice(n, DSCM_NOTICE_LEADERBOARD_STARTED, 7) != NULL, "notice: leaderboard started");
	x = find_notice(n, DSCM_NOTICE_LEADERBOARD_SUBMITTED, 7);
	fails += check(x && x->value == 150 && !strcmp(x->detail, "150"), "notice: leaderboard submitted, 150");
	int trackers = 0;
	for (int i = 0; i < DSCM_INDICATOR_MAX; i++) trackers += n->trackers[i].id != 0;
	fails += check(trackers == 0, "tracker hidden after submit");

	/* journal: the ARM7 copies it from RAM, so read it there */
	const dscm_journal_t* j = (const dscm_journal_t*)DSCM_UNCACHED(hdr->journal);
	int awards = 0, lbs = 0, unsent = 0;
	for (u32 i = 0; i < j->count && i < DSCM_JOURNAL_MAX; i++) {
		const dscm_journal_entry_t* e = &j->e[i];
		if (e->kind == DSCM_JOURNAL_AWARD && e->hardcore && e->unix_time >= 1700000000) awards++;
		if (e->kind == DSCM_JOURNAL_LEADERBOARD && e->id == 7 && e->score == 150) lbs++;
		unsent += !e->sent;
	}
	fails += check(j->magic == DSCM_JOURNAL_MAGIC && j->size == sizeof(dscm_journal_t) && j->game_id == 1 &&
		!strcmp(j->username, "tester") && !strcmp(j->game_hash, "00000000000000000000000000000000"), "journal header");
	siprintf(b, "journal: %lu entries, %d awards, %d leaderboard, %d unsent", (unsigned long)j->count, awards, lbs, unsent);
	fails += check(j->count == 5 && awards == 4 && lbs == 1 && unsent == 0, b);
	fails += check(lb_count == 1 && lb_scores[0] == 150, "leaderboard entry sent");
	return fails;
}

int main(void)
{
	if (!isDSiMode()) { out("FAIL needs DSi mode (16 MB)"); out("DONE"); while (1) swiWaitForVBlank(); }

	/* 1. load the module like the bootstrap loader */
	memcpy((void*)DSCM_BASE, dscheevos9_bin, dscheevos9_bin_size);
	DC_FlushAll(); IC_InvalidateAll();
	hdr = DSCM_HEADER;
	if (hdr->magic != DSCM_MAGIC || hdr->version != DSCM_VERSION) { out("FAIL bad module header"); goto end; }
	out("module image %lu bytes", (unsigned long)hdr->image_size);

	/* 2. session-start responses, as if fetched online before the game booted */
	object_ptr = 0x02000000 | ra(object);
	static char set[2560];
	int n = siprintf(set,
		"{\"Success\":true,\"GameId\":1,\"Title\":\"Module Test\",\"ConsoleId\":18,\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1,\"RichPresencePatch\":\"\","
		"\"Sets\":[{\"AchievementSetId\":1,\"GameId\":1,\"Title\":null,\"Type\":\"core\",\"ImageIconUrl\":\"\",\"Achievements\":["
		"{\"ID\":1,\"Title\":\"Level 3\",\"Description\":\"Reach level 3\",\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH%06lx=3\",\"Author\":\"t\",\"BadgeName\":\"0\",\"Created\":1,\"Modified\":1},"
		"{\"ID\":2,\"Title\":\"100 Coins\",\"Description\":\"\",\"Flags\":3,\"Points\":5,\"MemAddr\":\"0x %06lx>=100\",\"Author\":\"t\",\"BadgeName\":\"0\",\"Created\":1,\"Modified\":1},"
		"{\"ID\":3,\"Title\":\"Pointer\",\"Description\":\"\",\"Flags\":3,\"Points\":5,\"MemAddr\":\"I:0xX%06lx&33554431_0xH000005=42\",\"Author\":\"t\",\"BadgeName\":\"0\",\"Created\":1,\"Modified\":1},"
		"{\"ID\":4,\"Title\":\"Never\",\"Description\":\"\",\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH%06lx=99\",\"Author\":\"t\",\"BadgeName\":\"0\",\"Created\":1,\"Modified\":1},"
		"{\"ID\":5,\"Title\":\"Level 3 for 200 frames\",\"Description\":\"\",\"Flags\":3,\"Points\":10,\"MemAddr\":\"0xH%06lx=3.200.\",\"Author\":\"t\",\"BadgeName\":\"0\",\"Created\":1,\"Modified\":1}"
		"],\"Leaderboards\":[{\"ID\":7,\"Title\":\"Lap\",\"Description\":\"\",\"Mem\":\"STA:0xH%06lx=42::CAN:0=1::SUB:0xH%06lx=1::VAL:0x %06lx\",\"Format\":\"VALUE\",\"LowerIsBetter\":false,\"Hidden\":false}]}]}",
		(unsigned long)ra(&level), (unsigned long)ra(&coins), (unsigned long)ra(&object_ptr), (unsigned long)ra(&level), (unsigned long)ra(&level),
		(unsigned long)ra(&object[5]), (unsigned long)ra(&laps), (unsigned long)ra(&coins));
	const char* login = "{\"Success\":true,\"User\":\"tester\",\"Token\":\"tok\",\"Score\":0,\"Messages\":0}";
	const char* session = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
	/* session file layout: dscm_boot_t, then the responses */
	dscm_boot_t* boot = (dscm_boot_t*)DSCM_SESSION;
	memset(boot, 0, sizeof *boot);
	char* pre = (char*)DSCM_SESSION;
	u32 off = (sizeof *boot + 3) & ~3;
	const char* bodies[3] = {login, set, session};
	int lens[3] = {(int)strlen(login), n, (int)strlen(session)};
	for (int i = 0; i < 3; i++) { memcpy(pre + off, bodies[i], lens[i]); boot->preload_offset[i] = off; boot->preload_length[i] = lens[i]; off += (lens[i] + 3) & ~3; }
	boot->magic = DSCM_SESSION_MAGIC;
	boot->size = sizeof *boot;
	boot->file_size = off;
	strcpy(boot->rom_path, "sd:/test.nds");
	strcpy(boot->host, "http://mock"); strcpy(boot->username, "tester"); strcpy(boot->token, "tok");
	strcpy(boot->game_hash, "00000000000000000000000000000000");
	boot->hardcore = 1;
	boot->unix_time = 1700000000;
	DC_FlushAll();

	/* 3. init */
	int r = ((int (*)(const dscm_boot_t*))hdr->init)(boot);
	out("init -> %d, active %lu, error %lu, title \"%s\"", r, (unsigned long)hdr->status->active, (unsigned long)hdr->status->error, hdr->status->game_title);
	if (r != 0) goto end;

	/* 4. run frames: VBlank snapshots from the real IRQ, evaluation in idle time */
	irqSet(IRQ_VBLANK, vblank_irq);
	u32 unlock_frame[6] = {0};
	int reset_done = 0;
	while (frame < 600) {
		u32 f = frame;
		if (f >= 100 && level != 3) level = 3;
		if (f >= 200 && coins != 150) coins = 150;
		if (f >= 300 && object[5] != 42) object[5] = 42;   /* also starts leaderboard 7 */
		if (f >= 400 && laps != 1) laps = 1;               /* submits it: 150 coins */
		if (f >= 250 && !reset_done) {   /* the game resets: 5's hit count starts over */
			int ime = enterCriticalSection();
			((void (*)(void))hdr->reset)();
			leaveCriticalSection(ime);
			reset_done = 1;
		}
		((void (*)(void))hdr->halt)();     /* the game's patched OS_Halt: work, then wait for an IRQ */
		network_layer();
		for (u32 i = 0; i < hdr->status->unlock_count && i < DSCM_RECENT_MAX; i++) {
			u32 id = hdr->status->recent[i].id;
			if (id < 6 && !unlock_frame[id]) unlock_frame[id] = hdr->status->recent[i].frame;
		}
	}
	irqClear(IRQ_VBLANK);

	const dscm_status_t* st = hdr->status;
	out("frames: snapshotted %lu evaluated %lu dropped %lu missed %lu, idle pass ~%lu lines",
		(unsigned long)st->frames_snapshotted, (unsigned long)st->frames_evaluated, (unsigned long)st->frames_dropped,
		(unsigned long)st->frames_missed,
		(unsigned long)(st->eval_lines / 16));
	int fails = 0;
	struct { int id; u32 from, to; } expect[] = {{1, 100, 104}, {2, 200, 204}, {3, 300, 304}, {5, 448, 458}};
	for (int i = 0; i < 4; i++) {
		u32 f = unlock_frame[expect[i].id];
		int ok = f >= expect[i].from && f <= expect[i].to;
		out("%s: %d unlocked at frame %lu", ok ? "ok" : "FAIL", expect[i].id, (unsigned long)f);
		fails += !ok;
	}
	if (unlock_frame[4]) { out("FAIL: 4 must not unlock"); fails++; } else out("ok: 4 not unlocked");
	int hc = 0;
	for (int i = 0; i < award_count; i++) if (answered_awards[i] > 0) hc++;
	out("%s: %d awards sent, %d hardcore", award_count == 4 && hc == 4 ? "ok" : "FAIL", award_count, hc);
	if (!(award_count == 4 && hc == 4)) fails++;
	{
		int ok = hdr->notify_count == 4;
		out("%s: notify count %lu", ok ? "ok" : "FAIL", (unsigned long)hdr->notify_count);
		fails += !ok;
		ok = st->achievements == 5 && st->achievements_unlocked == 4 && st->points == 30 && st->points_unlocked == 25;
		out("%s: progress %lu/%lu, %lu/%lu pts", ok ? "ok" : "FAIL", (unsigned long)st->achievements_unlocked,
			(unsigned long)st->achievements, (unsigned long)st->points_unlocked, (unsigned long)st->points);
		fails += !ok;
	}
	if (st->frames_dropped || st->frames_missed) { out("FAIL: frames dropped or missed"); fails++; }
	fails += check_v6();
	out(fails ? "RESULT FAIL" : "RESULT PASS");
end:
	out("DONE");
	while (1) swiWaitForVBlank();
}
