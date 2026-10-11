# Builds the chainload test programs with BlocksDS:
#
#   docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src \
#     skylyrac/blocksds:slim-v1.24.0 make -C tests/chainload
#
# out/chainer.nds  boots sd:/target.nds via dscheevos/launcher/source/chainload.c
# out/target.nds   prints argc/argv, DSi mode and an SD read
# out/chainer-wifi.nds  same as chainer, with dswifi's ARM7 and Wi-Fi initialised
#
# load.bin comes from dscheevos/launcher/loader (devkitARM); run.sh uses both.

BLOCKSDS           ?= /opt/blocksds/core
WONDERFUL_TOOLCHAIN ?= /opt/wonderful
PREFIX   := $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-
CC       := $(PREFIX)gcc
NDSTOOL  := $(BLOCKSDS)/tools/ndstool/ndstool
SPECS    := $(BLOCKSDS)/sys/crts/ds_arm9.specs
ARM7ELF  ?= $(BLOCKSDS)/sys/arm7/main_core/arm7_maxmod.elf
LAUNCHER := ../../dscheevos/launcher/source

ARCH     := -mthumb -mcpu=arm946e-s+nofp
CFLAGS   := -Wall -Wextra -O2 $(ARCH) -ffunction-sections -fdata-sections \
            -specs=$(SPECS) -D__NDS__ -D__BLOCKSDS__ -DARM9 \
            -I$(BLOCKSDS)/libs/libnds/include -I$(LAUNCHER)
LIBS     := -lnds9 -lc
LDFLAGS   = $(ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib -L$(BLOCKSDS)/libs/dswifi/lib \
            -Wl,--gc-sections -Wl,--start-group $(LIBS) -Wl,--end-group

.PHONY: all clean
all: out/chainer.nds out/target.nds out/chainer-wifi.nds

out/chainer.elf: out/chainer/main.o out/chainer/chainload.o
out/target.elf: out/target/main.o
out/chainer-wifi.elf: out/chainer-wifi/main.o out/chainer/chainload.o
out/chainer-wifi.elf: LIBS := -ldswifi9 -lnds9 -lc
out/chainer-wifi.nds: ARM7ELF := $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi_maxmod.elf

out/chainer/chainload.o: $(LAUNCHER)/chainload.c $(LAUNCHER)/chainload.h
out/chainer/main.o: chainer/main.c $(LAUNCHER)/chainload.h
out/target/main.o: target/main.c
out/chainer-wifi/main.o: chainer/main.c $(LAUNCHER)/chainload.h
out/chainer-wifi/main.o: CFLAGS += -DWITH_WIFI -I$(BLOCKSDS)/libs/dswifi/include

out/%.o:
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

out/%.elf:
	$(CC) $^ -o $@ -Wl,-Map,$(@:.elf=.map) $(LDFLAGS)

out/%.nds: out/%.elf
	$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $< -b $(BLOCKSDS)/sys/icon.gif "DScheevos $*;chainload test;" > /dev/null
	@echo built $@

clean:
	rm -rf out
