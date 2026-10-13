# Read together with dscheevos/module/Makefile, so the core and rcheevos are
# compiled by the module's own rules and flags (see build.sh).
LAB_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
LAB_OBJ := $(BUILD)/dscheevos.o $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/rc/%.o,$(RC_SRC))
LIBNDS  ?= $(DEVKITPRO)/libnds

$(BUILD)/lab.o: $(LAB_DIR)main.c $(BUILD)/set-typical.h $(CORE)/include/dscheevos.h include/dscheevos_module.h
	$(CC) $(CFLAGS) -DARM9 -I$(LIBNDS)/include '-DSET_HEADER="$(BUILD)/set-typical.h"' -c $< -o $@

$(BUILD)/cpi-lab.elf: $(BUILD)/lab.o $(LAB_OBJ)
	$(CC) $(ARCH) -specs=ds_arm9.specs -Wl,--gc-sections $(LAB_LDFLAGS) $^ -L$(LIBNDS)/lib -lfat -lnds9 -lm -o $@

$(BUILD)/cpi-lab.nds: $(BUILD)/cpi-lab.elf
	ndstool -c $@ -9 $< > /dev/null
	@echo built $@
