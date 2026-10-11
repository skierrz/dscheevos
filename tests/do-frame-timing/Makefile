# Builds the do_frame timing test with devkitARM.
#
#   make RCHEEVOS=/path/to/rcheevos SET=typical MODE=thumb CPU=67
#
# SET   typical | heavy        synthetic set from tools/rcheevos-footprint
# MODE  thumb | arm            instruction set for rcheevos runtime+client
# CPU   67 | 133               ARM9 clock in DSi mode (DS mode is always 67)

ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM, or build inside the devkitpro/devkitarm Docker image")
endif
include $(DEVKITARM)/ds_rules

RCHEEVOS ?= ../../../rcheevos
SET      ?= typical
MODE     ?= thumb
CPU      ?= 67
NAME     := timing-$(SET)-$(MODE)-$(CPU)
OUT      := out/$(NAME)

GEN_typical := 60 8 1 3
GEN_heavy   := 250 40 2 6

RC_DEFS  := -DRC_NO_THREADS -DRC_CLIENT_SUPPORTS_HASH -DRC_HASH_NO_DISC \
            -DRC_HASH_NO_ZIP -DRC_HASH_NO_ENCRYPTED -DNDEBUG
ARCH     := -march=armv5te -mtune=arm946e-s -mthumb-interwork
COMMON   := $(ARCH) -Os -ffunction-sections -fdata-sections -DARM9 -std=gnu99 \
            -I$(RCHEEVOS)/include -I$(LIBNDS)/include -I$(OUT) $(RC_DEFS)
ifeq ($(MODE),arm)
HOT_ISA  := -marm
else
HOT_ISA  := -mthumb
endif
ifeq ($(CPU),133)
COMMON   += -DFAST_CPU
endif

RC_SRC   := \
            $(addprefix $(RCHEEVOS)/src/,client/rc_client.c \
              runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c \
              runtime/rc_consoleinfo.c runtime/rc_format.c runtime/rc_lboard.c \
              runtime/rc_memref.c runtime/rc_operand.c runtime/rc_richpresence.c \
              runtime/rc_runtime.c runtime/rc_runtime_progress.c runtime/rc_trigger.c \
              runtime/rc_value.c api/rc_api_common.c api/rc_api_info.c \
              api/rc_api_runtime.c api/rc_api_user.c hash/rc_hash.c hash/rc_hash_rom.c \
              util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c)
HOT      := client/rc_client.c runtime/%
RC_OBJ   := $(patsubst $(RCHEEVOS)/src/%.c,$(OUT)/rc/%.o,$(RC_SRC))

.PHONY: all clean
all: $(OUT).nds

$(OUT)/set.h:
	@mkdir -p $(OUT)
	python3 ../../tools/rcheevos-footprint/harness/gen_set.py $(GEN_$(SET)) > $@.tmp
	echo '#define SET_NAME "$(SET)"' >> $@.tmp
	echo '#define BUILD_NAME "$(MODE)"' >> $@.tmp
	mv $@.tmp $@

$(OUT)/rc/client/%.o $(OUT)/rc/runtime/%.o: ISA := $(HOT_ISA)
$(OUT)/rc/%.o: $(RCHEEVOS)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(or $(ISA),-mthumb) -c $< -o $@

$(OUT)/main.o: source/main.c $(OUT)/set.h
	$(CC) $(COMMON) -mthumb -Wall -c $< -o $@

$(OUT).elf: $(OUT)/main.o $(RC_OBJ)
	$(CC) -specs=ds_arm9.specs $(ARCH) -mthumb -Wl,--gc-sections -Wl,-Map,$(OUT).map \
	  $^ -L$(LIBNDS)/lib -lfat -lnds9 -lm -o $@

$(OUT).nds: $(OUT).elf
	ndstool -c $@ -9 $< -b $(LIBNDS)/icon.bmp "DScheevos timing;$(NAME);" > /dev/null
	@echo built $@

clean:
	rm -rf out
