# arm7-lab: rcheevos + the DScheevos core on the ARM7 (see README.md). Run from
# the repo root inside devkitpro/devkitarm:20241104 (tests/arm7-lab/build.sh).
#   RC_ISA   arm|thumb  rcheevos runtime + client (every-frame code); the rest is Thumb
#   RC_OPT   -Os|-O2    their optimization
#   OPT      -Os|-O2    everything else
#   FAST     none|hot|... fast-<name>.ld: input sections run from ARM7 WRAM
PREFIX   := $(DEVKITARM)/bin/arm-none-eabi-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
NM       := $(PREFIX)nm
LAB      := tests/arm7-lab
BUILD    ?= $(LAB)/out
RCHEEVOS := external/rcheevos
CORE     := dscheevos/core
LIBNDS   := $(DEVKITPRO)/libnds
RC_ISA   ?= arm
RC_OPT   ?= -Os
OPT      ?= -Os
FAST     ?= none
NWRAM    ?= 0
# NWRAM=1: per-set data in NWRAM-B/C (sys7.c); FAST region in NWRAM too (0x03100000, up to 256 KB)
NW_DEFS  := $(if $(filter 1,$(NWRAM)),-DLAB7_NWRAM)
NW_LINK  := $(if $(filter 1,$(NWRAM)),$(foreach w,malloc free calloc realloc,-Wl,--wrap=$(w)))

ARCH7    := -mcpu=arm7tdmi -mthumb -mthumb-interwork
CFLAGS7  := $(ARCH7) $(OPT) -ffunction-sections -fdata-sections -fno-strict-aliasing -Wall \
            -DRC_NO_THREADS -DNDEBUG -Idscheevos/module/include -I$(CORE)/include -I$(RCHEEVOS)/include -I$(RCHEEVOS)/src -I$(LAB) $(NW_DEFS)
RC_SRC := $(addprefix $(RCHEEVOS)/src/, client/rc_client.c \
  runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c \
  runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c \
  runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c \
  runtime/rc_trigger.c runtime/rc_value.c \
  api/rc_api_common.c api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c \
  util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c)
RC_HOT   := client/% runtime/%
RC_HOTFL := $(if $(filter arm,$(RC_ISA)),-marm,-mthumb) $(RC_OPT)
P7OBJ    := $(BUILD)/p7/start7.o $(BUILD)/p7/bench7.o $(BUILD)/p7/sys7.o $(BUILD)/p7/dscheevos.o \
            $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/p7/rc/%.o,$(RC_SRC))

.PHONY: all
all: $(BUILD)/arm7-lab.nds

$(BUILD)/p7/rc/%.o: $(RCHEEVOS)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS7) $(if $(filter $(RC_HOT),$*),$(RC_HOTFL)) -w -c $< -o $@
$(BUILD)/p7/dscheevos.o: $(CORE)/src/dscheevos.c $(CORE)/include/dscheevos.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS7) -c $< -o $@
$(BUILD)/p7/%.o: $(LAB)/%.c $(LAB)/lab7.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS7) -I$(LIBNDS)/include -c $< -o $@
$(BUILD)/p7/start7.o: $(LAB)/start7.s
	@mkdir -p $(dir $@)
	$(CC) $(ARCH7) -c $< -o $@

# the ARM7 program (libnds services + the payload runner), in WRAM
$(BUILD)/arm7.elf: $(LAB)/arm7.c $(LAB)/lab7.h
	@mkdir -p $(BUILD)
	$(CC) $(ARCH7) -Os -DARM7 -I$(LIBNDS)/include -I$(LAB) -specs=ds_arm7.specs $< -L$(LIBNDS)/lib -lnds7 -o $@

# fast region: from the ARM7 program's end to 0x0380C000 (the rest is its stacks)
$(BUILD)/p7/payload.ld: $(LAB)/payload.ld.in $(LAB)/fast-$(FAST).ld $(BUILD)/arm7.elf
	@mkdir -p $(dir $@)
	end=$$($(NM) $(BUILD)/arm7.elf | awk '$$3=="__end__"{print $$1}'); \
	org=$$(printf '0x%08X' $$(( (0x$$end + 0xFF) & ~0xFF ))); \
	len=$$(printf '0x%X' $$(( 0x0380C000 - $$org ))); \
	bss=image; hs=__bss_end; \
	if [ "$(NWRAM)" = 1 ]; then org=0x03100000; len=0x80000; bss=fast; hs='ALIGN(__image_end, 32)'; fi; \
	sed -e "s/@FAST_ORIGIN@/$$org/" -e "s/@FAST_LENGTH@/$$len/" -e "s/@BSS_REGION@/$$bss/" -e "s/@HEAP_START@/$$hs/" $< > $@; \
	echo "fast region $$org, $$len bytes"
	cp $(LAB)/fast-$(FAST).ld $(BUILD)/p7/fast.ld

$(BUILD)/payload.elf: $(P7OBJ) $(BUILD)/p7/payload.ld
	$(CC) $(ARCH7) -nostartfiles -T $(BUILD)/p7/payload.ld -L$(BUILD)/p7 -Wl,--gc-sections $(NW_LINK) -Wl,-Map,$(BUILD)/payload.map \
	  $(P7OBJ) -lc -lm -lgcc -o $@
	$(PREFIX)size -A $@ | awk '$$2>0 && /^\./'

$(BUILD)/payload.bin: $(BUILD)/payload.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/arm9.o: $(LAB)/arm9.c $(LAB)/lab7.h $(BUILD)/set-typical.h
	$(CC) -march=armv5te -mtune=arm946e-s -mthumb -mthumb-interwork -Os -Wall -DARM9 -I$(LIBNDS)/include -I$(LAB) \
	  '-DSET_HEADER="$(abspath $(BUILD))/set-typical.h"' -c $< -o $@
$(BUILD)/payload_bin.o: $(BUILD)/payload.bin
	printf '.section .rodata\n.balign 4\n.global payload_bin, payload_bin_end\npayload_bin:\n.incbin "%s"\npayload_bin_end:\n' $< > $(BUILD)/payload_bin.s
	$(CC) -c $(BUILD)/payload_bin.s -o $@
$(BUILD)/arm9.elf: $(BUILD)/arm9.o $(BUILD)/payload_bin.o
	$(CC) -march=armv5te -mthumb -mthumb-interwork -specs=ds_arm9.specs $^ -L$(LIBNDS)/lib -lfat -lnds9 -o $@

$(BUILD)/arm7-lab.nds: $(BUILD)/arm9.elf $(BUILD)/arm7.elf
	$(DEVKITPRO)/tools/bin/ndstool -c $@ -9 $(BUILD)/arm9.elf -7 $(BUILD)/arm7.elf > /dev/null
	@echo built $@

# ---- split mode (arm7-split.nds): the ARM9 runs the core + rc_client with the
# trigger evaluator (DSC_TRIGGER_EVALUATOR), built with the module's flags
# (dscheevos/module/Makefile as of 1c0ee73: Thumb -Os, rcheevos runtime and
# client ARM -O2); the ARM7 payload above evaluates the claimed triggers.
ARCH9    := -march=armv5te -mtune=arm946e-s -mthumb -mthumb-interwork
CFLAGS9  := $(ARCH9) -Os -ffunction-sections -fdata-sections -fno-strict-aliasing -Wall \
            -DRC_NO_THREADS -DNDEBUG -DDSC_TRIGGER_EVALUATOR -DARM9 -Idscheevos/module/include -I$(CORE)/include \
            -I$(RCHEEVOS)/include -I$(RCHEEVOS)/src -I$(LAB) -I$(LIBNDS)/include
S9OBJ    := $(BUILD)/s9/split9.o $(BUILD)/s9/dscheevos.o $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/s9/rc/%.o,$(RC_SRC))

$(BUILD)/s9/rc/%.o: $(RCHEEVOS)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS9) $(if $(filter $(RC_HOT),$*),-marm -O2) -w -c $< -o $@
$(BUILD)/s9/dscheevos.o: $(CORE)/src/dscheevos.c $(CORE)/include/dscheevos.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS9) -c $< -o $@
$(BUILD)/s9/split9.o: $(LAB)/split9.c $(LAB)/lab7.h $(CORE)/include/dscheevos.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS9) -c $< -o $@
$(BUILD)/split9.elf: $(S9OBJ) $(BUILD)/payload_bin.o
	$(CC) $(ARCH9) -specs=ds_arm9.specs -Wl,--gc-sections $^ -L$(LIBNDS)/lib -lfat -lnds9 -lm -o $@
$(BUILD)/arm7-split.nds: $(BUILD)/split9.elf $(BUILD)/arm7.elf
	$(DEVKITPRO)/tools/bin/ndstool -c $@ -9 $(BUILD)/split9.elf -7 $(BUILD)/arm7.elf > /dev/null
	@echo built $@
