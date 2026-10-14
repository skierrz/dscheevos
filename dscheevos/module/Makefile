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
LWIP     ?= ../../external/lwip
MBEDTLS  ?= ../../external/mbedtls
TLSCONF  := ../launcher/tls
BUILD    ?= build

ARCH     := -march=armv5te -mtune=arm946e-s -mthumb -mthumb-interwork
CFLAGS   := $(ARCH) -Os -ffunction-sections -fdata-sections -fno-strict-aliasing -Wall \
            -DRC_NO_THREADS -DNDEBUG -Iinclude -I$(CORE)/include -I$(RCHEEVOS)/include -I$(RCHEEVOS)/src \
            -Isrc/net -I$(LWIP)/src/include -I$(TLSCONF) -I$(MBEDTLS)/include '-DMBEDTLS_CONFIG_FILE="dsc_mbedtls_config.h"' -DDSC_TLS_DER_ONLY
VERSION  := $(shell cat ../../VERSION)
CFLAGS   += '-DDSC_VERSION="$(VERSION)"'
# evaluation build options, e.g. DSC_FLAGS="-DDSC_CHANGE_DRIVEN -DDSC_CHANGED_RANGES" (rcheevos
# patches on our fork, opt-in; docs/rcheevos-pin.md). Empty by default.
CFLAGS   += $(DSC_FLAGS)
# header dependencies (build/*.d): a changed header rebuilds every object that includes it
CFLAGS   += -MMD -MP $(EXTRA_CFLAGS)
# The module's data (statics, the snapshot arena, the session, small sets' heap) through the
# 0x0C000000 mirror, which most games' MPUs leave uncached: our data never evicts the game's
# cache lines (tests/perf/AB.md "Data through the mirror"). DATA_MIRROR=0: the cached layout.
DATA_MIRROR ?= 1
ifeq ($(DATA_MIRROR),1)
CFLAGS   += -DDSCM_DATA_MIRROR
endif
# mbedTLS's calloc/free set at run time (src/net/https.c: through the cache)
CFLAGS   += -DMBEDTLS_PLATFORM_MEMORY
# heap.c places load-time memory (heap.h): it wraps malloc and rc_api's parse of the set
# fast_strtoul.c: newlib's strtoul without its per-call divisions
WRAPS    := malloc free calloc realloc rc_api_process_fetch_game_sets_server_response strtoul
comma    := ,
LDFLAGS  := $(if $(TEXT_COLOUR),-Wl$(comma)--defsym$(comma)DSCM_TEXT_COLOUR=$(TEXT_COLOUR)) \
            $(if $(DATA_COLOUR),-Wl$(comma)--defsym$(comma)DSCM_DATA_COLOUR=$(DATA_COLOUR)) \
            $(ARCH) -nostartfiles -T $(or $(LDSCRIPT),$(if $(filter 1,$(DATA_MIRROR)),module.ld,module-cached.ld)) -Wl,--use-blx -Wl,--gc-sections -Wl,-Map,$(BUILD)/dscheevos9.map \
            $(foreach w,$(WRAPS),-Wl,--wrap=$(w)) $(EXTRA_LDFLAGS)

RC_SRC := $(addprefix $(RCHEEVOS)/src/, client/rc_client.c \
  runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c \
  runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c \
  runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c \
  runtime/rc_trigger.c runtime/rc_value.c \
  api/rc_api_common.c api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c \
  util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c)
LWIP_SRC := $(addprefix $(LWIP)/src/, core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c \
  core/mem.c core/memp.c core/netif.c core/pbuf.c core/stats.c core/sys.c core/tcp.c core/tcp_in.c \
  core/tcp_out.c core/timeouts.c core/udp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c \
  core/ipv4/ip4_addr.c core/ipv4/ip4_frag.c netif/ethernet.c)
TLS_SRC := $(wildcard $(MBEDTLS)/library/*.c)
NET_SRC := $(wildcard src/net/*.c) $(TLSCONF)/cacerts.c
SRC := src/module.c src/syscalls.c src/heap.c src/fast_strtoul.c $(CORE)/src/dscheevos.c
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRC))) $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/rc/%.o,$(RC_SRC)) $(BUILD)/start.o \
  $(patsubst $(LWIP)/src/%.c,$(BUILD)/lwip/%.o,$(LWIP_SRC)) $(patsubst $(MBEDTLS)/library/%.c,$(BUILD)/tls/%.o,$(TLS_SRC)) \
  $(patsubst %.c,$(BUILD)/net/%.o,$(notdir $(NET_SRC)))

.PHONY: all clean
all: $(BUILD)/dscheevos9.bin

# flags live here: a Makefile change rebuilds everything
$(OBJ): Makefile

$(BUILD)/%.o: src/%.c src/heap.h include/dscheevos_module.h $(CORE)/include/dscheevos.h ../../VERSION
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Code that runs every frame is ARM, not Thumb: rcheevos' runtime and client
# (Thumb-1 has no jump tables or three-operand ALU instructions; about 30%
# fewer instructions per frame evaluated, docs/rcheevos-arm9-footprint.md).
# The core marks its own hot functions ARM (HOT in dscheevos.c).
RC_ARM   := client/% runtime/%
RC_HOT_FLAGS ?= -marm -O2
# per file, e.g. RC_FLAGS_runtime_rc_condset := -O2 (after RC_HOT_FLAGS, so it wins)
$(BUILD)/dscheevos.o: $(CORE)/src/dscheevos.c $(CORE)/include/dscheevos.h include/dscheevos_module.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/rc/%.o: $(RCHEEVOS)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter $(RC_ARM),$*),$(RC_HOT_FLAGS)) $(RC_FLAGS_$(subst /,_,$*)) -w -c $< -o $@
$(BUILD)/lwip/%.o: $(LWIP)/src/%.c src/net/lwipopts.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@
$(BUILD)/tls/%.o: $(MBEDTLS)/library/%.c $(TLSCONF)/dsc_mbedtls_config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter bignum% ecp% ecdsa ecdh,$*),-marm -O2) -w -c $< -o $@
$(BUILD)/net/%.o: src/net/%.c src/net/net.h include/dscheevos_module.h include/dscm_shim.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/net/cacerts.o: $(TLSCONF)/cacerts.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/start.o: src/start.s
	@mkdir -p $(BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BUILD)/dscheevos9.elf: $(OBJ) module.ld
	$(CC) $(LDFLAGS) $(OBJ) -lc -lm -lgcc -o $@
	$(SIZE) $@

$(BUILD)/dscheevos9.bin: $(BUILD)/dscheevos9.elf
	$(OBJCOPY) -O binary $< $@
	@echo "built $@ ($$(stat -c %s $@) bytes)"

-include $(OBJ:.o=.d)

clean:
	rm -rf $(BUILD)
