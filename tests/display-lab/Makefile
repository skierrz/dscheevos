# display-lab: DSi hardware tests for the notice strip (docs/display-hw-tests.md).
# Build in the devkitARM image nds-bootstrap uses (from the repo root):
#   docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 \
#     make -C tests/display-lab BOOTSTRAP=/src/external/nds-bootstrap
include $(DEVKITARM)/ds_rules

BOOTSTRAP ?= ../../external/nds-bootstrap
DISPLAY   := ../../dscheevos/display
OUT       := out
ARCH9     := -march=armv5te -mtune=arm946e-s -mthumb -mthumb-interwork
ARCH7     := -mcpu=arm7tdmi -mtune=arm7tdmi -mthumb -mthumb-interwork
CFLAGS9   := $(ARCH9) -O2 -Wall -DARM9 -I$(LIBNDS)/include -I$(DISPLAY)/include -I../../dscheevos/module/include
CFLAGS7   := $(ARCH7) -O2 -Wall -DARM7 -I$(LIBNDS)/include
SRC9      := arm9/main.c $(DISPLAY)/src/dsd_sched.c $(DISPLAY)/src/dsd_render.c $(OUT)/font_latin.c

all: $(OUT)/display-lab.nds

$(OUT)/font_latin.c: $(DISPLAY)/tools/fontgen.py
	@mkdir -p $(OUT)
	python3 $(DISPLAY)/tools/fontgen.py $(BOOTSTRAP) extended_latin dsd_font_latin $@

$(OUT)/arm9.elf: $(SRC9) $(DISPLAY)/include/dsd.h
	$(CC) $(CFLAGS9) -specs=ds_arm9.specs $(SRC9) -L$(LIBNDS)/lib -lfat -lnds9 -o $@

$(OUT)/arm7.elf: arm7/main.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS7) -specs=ds_arm7.specs arm7/main.c -L$(LIBNDS)/lib -lnds7 -o $@

$(OUT)/display-lab.nds: $(OUT)/arm9.elf $(OUT)/arm7.elf
	ndstool -c $@ -9 $(OUT)/arm9.elf -7 $(OUT)/arm7.elf > /dev/null
	@echo built $@

clean:
	rm -rf $(OUT)
