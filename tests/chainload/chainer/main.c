/*
 * Chainload test, first program: boots argv[1] (default sd:/target.nds)
 * through dscheevos/launcher/source/chainload.c, passing it argv
 * {path, "hello"}. Reports over the no$gba debug port ("@@" lines).
 */
#include <nds.h>
#include <fat.h>
#include <stdarg.h>
#include <stdio.h>

#include "chainload.h"

#ifdef WITH_WIFI
#include <dswifi9.h>
#endif

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
	const char* path = argc > 1 ? argv[1] : "sd:/target.nds";
	const char* args[] = { path, "hello" };

	if (!fatInitDefault()) {
		dbg("@@CHAINER fatInitDefault failed\n");
		dbg("@@DONE\n");
		for (;;) swiWaitForVBlank();
	}
#ifdef WITH_WIFI
	/* The launcher will have Wi-Fi up when it boots nds-bootstrap. */
	dbg("@@CHAINER wifi_init=%d\n", Wifi_InitDefault(INIT_ONLY));
#endif
	dbg("@@CHAINER dsi=%d booting %s\n", isDSiMode(), path);

	int rc = chainload(path, 2, args);

	dbg("@@CHAINER chainload failed: %d\n", rc);
	dbg("@@DONE\n");
	for (;;) swiWaitForVBlank();
}
