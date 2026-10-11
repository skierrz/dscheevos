# Builds load.bin, the ARM7 chainloader used by the launcher's chainload().
#
# load.bin is devkitPro's nds-bootloader (GPLv2+, external/nds-bootloader).
# It is shipped as a separate file (sd:/_nds/dscheevos/load.bin) and read at
# runtime; it is never linked into the launcher.
#
# Upstream master needs libnds 2 / calico, so build it in a devkitARM image
# from 2025 or later (devkitpro/devkitarm:20241104 has libnds 1.8 and fails):
#
#   docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src \
#     devkitpro/devkitarm:20260610 make -C dscheevos/launcher/loader
#
# The submodule is copied into out/ before building so its tree stays clean.

ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM, or build inside the devkitpro/devkitarm Docker image")
endif

SRC := ../../../external/nds-bootloader
OUT := out

SRC_FILES := $(SRC)/Makefile $(SRC)/load.ld $(wildcard $(SRC)/source/* $(SRC)/arm9code/*)

.PHONY: all clean
all: $(OUT)/load.bin

$(OUT)/load.bin: $(SRC_FILES)
	@test -f $(SRC)/Makefile || { echo "run: git submodule update --init external/nds-bootloader"; exit 1; }
	rm -rf $(OUT)/src
	mkdir -p $(OUT)/src
	cp -r $(SRC)/Makefile $(SRC)/load.ld $(SRC)/source $(SRC)/arm9code $(OUT)/src/
	$(MAKE) -C $(OUT)/src
	cp $(OUT)/src/load.bin $@
	@echo built $@

clean:
	rm -rf $(OUT)
