/*
 * Boot another .nds file from the DSi SD card, with argv (BlocksDS, ARM9).
 *
 * Works like the "run" path of hbmenu / TWiLight Menu++: the ARM7 is reset
 * into devkitPro's nds-bootloader (load.bin, loaded at runtime from the SD
 * card), which reads the target .nds from the SD card, loads its ARM9/ARM7
 * (and DSi ARM9i/ARM7i) binaries, writes argv to the libnds '_arg' block at
 * 0x02FFFE70 and starts both CPUs. In DSi mode the target starts in DSi
 * mode, with SCFG left as the caller had it (the launcher must not lock it).
 */
#ifndef DSCHEEVOS_CHAINLOAD_H
#define DSCHEEVOS_CHAINLOAD_H

/* Where the launcher ships the loader. Override at build time if needed. */
#ifndef CHAINLOAD_LOADER_PATH
#define CHAINLOAD_LOADER_PATH "sd:/_nds/dscheevos/load.bin"
#endif

enum chainload_error {
	CHAINLOAD_ERR_UNSUPPORTED = -1, /* not DSi mode, or target not on sd: */
	CHAINLOAD_ERR_TARGET      = -2, /* target .nds not found (stat failed) */
	CHAINLOAD_ERR_LOADER      = -3, /* load.bin missing, unreadable or invalid */
	CHAINLOAD_ERR_ARGS        = -4, /* argv does not fit in VRAM bank C */
	CHAINLOAD_ERR_MEMORY      = -5, /* out of memory */
};

/*
 * Boot the .nds file at path ("sd:/..."). argv[0] should be the same path,
 * the way hbmenu passes it; if argc <= 0 or argv is NULL, {path} is used.
 * Never returns on success; on failure returns a chainload_error and leaves
 * the caller running.
 */
int chainload(const char* path, int argc, const char** argv);

#endif
