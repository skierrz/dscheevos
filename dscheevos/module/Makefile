# DScheevos ARM9 module (dscheevos9.bin). Build with devkitARM:
#   docker run --rm -v $PWD:/repo -w /repo/dscheevos/module devkitpro/devkitarm:20241104 make
ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM, or build inside the devkitpro/devkitarm Docker image")
endif

PREFIX   := $(DEVKITARM)/bin/arm-none-eabi-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
SIZE     := $(PREFIX)size
RCHEEVOS ?= ../../external/rcheevos
CORE     := ../core
BUILD    := build

ARCH     := -march=armv5te -mtune=arm946e-s -mthumb -mthumb-interwork
CFLAGS   := $(ARCH) -Os -ffunction-sections -fdata-sections -fno-strict-aliasing -Wall \
            -DRC_NO_THREADS -DNDEBUG -Iinclude -I$(CORE)/include -I$(RCHEEVOS)/include -I$(RCHEEVOS)/src
LDFLAGS  := $(ARCH) -nostartfiles -T module.ld -Wl,--gc-sections -Wl,-Map,$(BUILD)/dscheevos9.map

RC_SRC := $(addprefix $(RCHEEVOS)/src/, client/rc_client.c \
  runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c \
  runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c \
  runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c \
  runtime/rc_trigger.c runtime/rc_value.c \
  api/rc_api_common.c api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c \
  util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c)
SRC := src/module.c src/syscalls.c $(CORE)/src/dscheevos.c
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRC))) $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/rc/%.o,$(RC_SRC)) $(BUILD)/start.o

.PHONY: all clean
all: $(BUILD)/dscheevos9.bin

$(BUILD)/%.o: src/%.c include/dscheevos_module.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/dscheevos.o: $(CORE)/src/dscheevos.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/rc/%.o: $(RCHEEVOS)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@
$(BUILD)/start.o: src/start.s
	@mkdir -p $(BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BUILD)/dscheevos9.elf: $(OBJ) module.ld
	$(CC) $(LDFLAGS) $(OBJ) -lc -lm -lgcc -o $@
	$(SIZE) $@

$(BUILD)/dscheevos9.bin: $(BUILD)/dscheevos9.elf
	$(OBJCOPY) -O binary $< $@
	@echo "built $@ ($$(stat -c %s $@) bytes)"

clean:
	rm -rf $(BUILD)
