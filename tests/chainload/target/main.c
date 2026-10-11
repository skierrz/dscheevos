/*
 * Chainload test, second program: prints what it was started with.
 * Lines on the no$gba debug port: "@@TARGET ...", then "@@DONE".
 */
#include <nds.h>
#include <fat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* melonDS reads debug strings over the bus (no cache, no DTCM): use a static
 * main-RAM buffer and flush it. */
static char dbg_line[160] __attribute__((aligned(32)));
static void dbg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dbg(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(dbg_line, sizeof dbg_line, fmt, ap);
	va_end(ap);
	DC_FlushRange(dbg_line, sizeof dbg_line);
	nocashMessage(dbg_line);
	printf("%s", dbg_line + 2);
}

int main(int argc, char** argv)
{
	consoleDemoInit();

	dbg("@@TARGET argc=%d\n", argc);
	for (int i = 0; i < argc; i++)
		dbg("@@TARGET argv[%d]=%s\n", i, argv[i]);
	dbg("@@TARGET dsi=%d scfg_ext=%08lx\n", isDSiMode(),
	    isDSiMode() ? (unsigned long)REG_SCFG_EXT : 0ul);

	int fat = fatInitDefault();
	FILE* f = fat ? fopen("sd:/chainload-test.txt", "rb") : NULL;
	char text[64] = "";
	if (f) {
		size_t n = fread(text, 1, sizeof text - 1, f);
		text[n] = 0;
		text[strcspn(text, "\r\n")] = 0;
		fclose(f);
	}
	dbg("@@TARGET fat=%d sd_read=%s text=%s\n", fat, f ? "ok" : "fail", text);
	dbg("@@DONE\n");
	for (;;) swiWaitForVBlank();
}
