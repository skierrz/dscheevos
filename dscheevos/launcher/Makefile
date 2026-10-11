# DScheevos launcher (BlocksDS). Build:
#   docker run --rm -v $PWD:/repo -w /repo/dscheevos/launcher skylyrac/blocksds:slim-latest make
NAME          := dscheevos-launcher
GAME_TITLE    := DScheevos
GAME_SUBTITLE := RetroAchievements for DS
GAME_AUTHOR   := DScheevos

SOURCEDIRS    := source rcheevos tls
INCLUDEDIRS   := ../module/include ../../external/rcheevos/include ../../external/rcheevos/src \
                 tls ../../external/mbedtls/include
DEFINES       := -DRC_NO_THREADS -DRC_CLIENT_SUPPORTS_HASH -DRC_HASH_NO_DISC -DRC_HASH_NO_ZIP \
                 -DRC_HASH_NO_ENCRYPTED -DNDEBUG '-DMBEDTLS_CONFIG_FILE="dsc_mbedtls_config.h"'
ARM7ELF       := $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
LIBS          := -ldswifi9 -lnds9
LIBDIRS       := $(BLOCKSDS)/libs/dswifi $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
