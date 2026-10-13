# Module test .nds (DSi mode). Build the module first (dscheevos/module).
ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM, or build inside the devkitpro/devkitarm Docker image")
endif
include $(DEVKITARM)/ds_rules
MODULE := ../../dscheevos/module
OUT ?= out
# the module image to test (SCFG_PROBE diagnostic build: MODULE_BIN=$(MODULE)/build-scfgprobe/dscheevos9.bin OUT=out-probe)
MODULE_BIN ?= $(MODULE)/build/dscheevos9.bin
CFLAGS := -march=armv5te -mtune=arm946e-s -mthumb -mthumb-interwork -Os -Wall -DARM9 -I$(LIBNDS)/include -I$(MODULE)/include -I$(OUT)
all: $(OUT)/module-test.nds
$(OUT)/dscheevos9_bin.h $(OUT)/dscheevos9_bin.o: $(MODULE_BIN)
	@mkdir -p $(OUT)
	cp $< $(OUT)/dscheevos9.bin
	cd $(OUT) && bin2s -a 32 -H dscheevos9_bin.h dscheevos9.bin > dscheevos9_bin.s && $(CC) -c dscheevos9_bin.s -o dscheevos9_bin.o
$(OUT)/main.o: source/main.c $(OUT)/dscheevos9_bin.h
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/idle.o: source/idle.s
	@mkdir -p $(OUT)
	$(CC) -march=armv5te -c $< -o $@
$(OUT)/module-test.elf: $(OUT)/main.o $(OUT)/idle.o $(OUT)/dscheevos9_bin.o
	$(CC) -specs=ds_arm9.specs -mthumb -mthumb-interwork $^ -L$(LIBNDS)/lib -lnds9 -o $@
$(OUT)/module-test.nds: $(OUT)/module-test.elf
	ndstool -c $@ -9 $< > /dev/null
	@echo built $@
clean:
	rm -rf $(OUT)
