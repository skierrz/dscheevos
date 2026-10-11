/*
 * Boot another .nds file from the DSi SD card, with argv (BlocksDS, ARM9).
 *
 * Handoff, as in hbmenu's / TWiLight Menu++'s runNds():
 *
 *  1. Read load.bin (devkitPro nds-bootloader) from the SD card and stat()
 *     the target; BlocksDS puts the file's first FAT cluster in st_ino.
 *  2. Map VRAM bank C to the ARM9 (LCDC), copy the loader there, fill its
 *     parameter block and append the argv strings.
 *  3. Give bank C to the ARM7 at 0x06000000 and park the ARM9 in the BIOS
 *     "passme" loop at 0x02FFFE04 (ldr pc,[0x02FFFE24]) with
 *     0x02FFFE24 = 0x02FFFE04.
 *  4. resetARM7(0x06000000): the default BlocksDS ARM7 handles the FIFO
 *     reset command and jumps there through swiSoftReset.
 *
 * The loader then drives the ARM9 by writing entry points to 0x02FFFE24,
 * so the ARM9 must see those writes. BlocksDS caches all of main RAM
 * (unlike the old libnds MPU layout these loaders were written for, which
 * left 0x02FFF000 uncached), so the ARM9 cleans and disables its caches and
 * MPU before entering the loop. The loader's own ARM9 stub sets up a fresh
 * MPU layout before starting the target.
 */
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "chainload.h"

/* Parameter block at the start of load.bin (load_crt0.s in nds-bootloader). */
typedef struct {
	u32 branch;            /* b startUp */
	u32 storedFileCluster; /* first FAT cluster of the .nds to boot */
	u32 wantToPatchDLDI;   /* 0: the DSi SD driver is built in */
	u32 argStart;          /* offset of the argv strings from the loader start */
	u32 argSize;           /* bytes of argv strings, NULs included */
	u32 dldiOffset;
	u32 dsiSD;             /* boot from the DSi SD slot */
	u32 dsiMode;           /* DSi mode: load ARM9i/ARM7i, keep SCFG */
} loader_header;

#define LOADER_BRANCH     0xEA000006u /* b startUp, right after the block */
#define VRAM_C_BYTES      (128 * 1024)
#define ARM7_LOADER_ENTRY 0x06000000u

#define ARM9_LOOP         ((vu32*)0x02FFFE04)
#define ARM9_LOOP_TARGET  ((vu32*)0x02FFFE24)
#define LDR_PC_LOOP       0xE59FF018u /* ldr pc, [pc, #0x18] -> [0x02FFFE24] */

/* VRAM takes no 8-bit writes; copy words. */
static void copy_words(volatile u32* dst, const void* src, size_t bytes)
{
	const u32* s = src;
	for (size_t i = 0; i < bytes / 4; i++)
		dst[i] = s[i];
}

/* Whole file into a word-padded, zero-filled buffer. */
static u32* read_file(const char* path, size_t* size)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	u32* buf = NULL;
	long len = -1;
	if (fseek(f, 0, SEEK_END) == 0)
		len = ftell(f);
	if (len > 0 && len <= VRAM_C_BYTES && fseek(f, 0, SEEK_SET) == 0) {
		buf = calloc(1, (len + 3) & ~3);
		if (buf && fread(buf, 1, len, f) != (size_t)len) {
			free(buf);
			buf = NULL;
		}
	}
	fclose(f);
	*size = buf ? (size_t)len : 0;
	return buf;
}

int chainload(const char* path, int argc, const char** argv)
{
	const char* self_argv[1] = { path };
	if (argc <= 0 || !argv) {
		argc = 1;
		argv = self_argv;
	}

	/* Only the DSi SD path is supported: a DS-mode flashcard boot would also
	 * need the loader DLDI-patched with the card's driver. */
	if (!isDSiMode() || strncmp(path, "sd:", 3) != 0)
		return CHAINLOAD_ERR_UNSUPPORTED;

	struct stat st;
	if (stat(path, &st) != 0 || st.st_ino < 2)
		return CHAINLOAD_ERR_TARGET;

	size_t loader_size;
	u32* loader = read_file(CHAINLOAD_LOADER_PATH, &loader_size);
	if (!loader)
		return CHAINLOAD_ERR_LOADER;
	loader_header* hdr = (loader_header*)loader;
	if (loader_size < sizeof *hdr || hdr->branch != LOADER_BRANCH
	    || hdr->argStart < loader_size || hdr->argStart >= VRAM_C_BYTES) {
		free(loader);
		return CHAINLOAD_ERR_LOADER;
	}

	/* argv as consecutive NUL-terminated strings, padded to words. */
	size_t arg_size = 0;
	for (int i = 0; i < argc && argv[i]; i++)
		arg_size += strlen(argv[i]) + 1;
	u32 arg_start = (hdr->argStart + 3) & ~3u;
	if (arg_start + arg_size > VRAM_C_BYTES) {
		free(loader);
		return CHAINLOAD_ERR_ARGS;
	}
	char* args = calloc(1, (arg_size + 3) & ~3u);
	if (!args) {
		free(loader);
		return CHAINLOAD_ERR_MEMORY;
	}
	for (size_t i = 0, pos = 0; i < (size_t)argc && argv[i]; i++) {
		size_t n = strlen(argv[i]) + 1;
		memcpy(args + pos, argv[i], n);
		pos += n;
	}

	hdr->storedFileCluster = (u32)st.st_ino;
	hdr->wantToPatchDLDI = 0;
	hdr->argStart = arg_start;
	hdr->argSize = arg_size;
	hdr->dsiSD = 1;
	hdr->dsiMode = 1;

	/* Nothing may interrupt the handoff from here on. */
	REG_IME = 0;
	REG_IE = 0;
	REG_IF = ~0u;

	VRAM_C_CR = VRAM_ENABLE | VRAM_C_LCD;
	copy_words((vu32*)VRAM_C, loader, (loader_size + 3) & ~3u);
	copy_words((vu32*)((u8*)VRAM_C + arg_start), args, (arg_size + 3) & ~3u);
	free(args);
	free(loader);
	VRAM_C_CR = VRAM_ENABLE | VRAM_C_ARM7_0x06000000;

	/* As hbmenu/TWiLight: slot-1/2 buses to the ARM7, clear the main-memory
	 * command word, and park the ARM9 in the passme loop. */
	REG_EXMEMCNT |= ARM7_OWNS_ROM | ARM7_OWNS_CARD;
	*(vu32*)0x02FFFFFC = 0;
	*ARM9_LOOP = LDR_PC_LOOP;
	*ARM9_LOOP_TARGET = (u32)ARM9_LOOP;

	/* BlocksDS shrinks the RAM setting to 16 MB on retail units. Put back
	 * the launcher's 32 MB setting (as BlocksDS's own exit() does) for
	 * programs that read SCFG_EXT to size RAM. */
	if (REG_SCFG_EXT & BIT(31))
		REG_SCFG_EXT |= SCFG_EXT_RAM_DEBUG | SCFG_EXT_RAM_TWL;

	/* Clean + disable data/instruction caches and the MPU, so the writes
	 * above reach RAM and the ARM9 sees the loader's writes to 0x02FFFE24.
	 * The stack stays in DTCM, which is unaffected. */
	CP15_MPUDisable();

	resetARM7(ARM7_LOADER_ENTRY);

	((void (*)(void))(u32)ARM9_LOOP)();
	for (;;)
		;
}
