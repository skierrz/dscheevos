# DScheevos ARM9 module (dscheevos9.bin). Build with devkitARM:
#   docker run --rm -v $PWD:/repo -w /repo/dscheevos/module devkitpro/devkitarm:20241104 make
ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM, or build inside the devkitpro/devkitarm Docker image")
endif

PREFIX   := $(DEVKITARM)/bin/arm-none-eabi-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
SIZE     := $(PREFIX)size
NM       := $(PREFIX)nm
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
# evaluation build options: change-driven evaluation and changed ranges (rcheevos patches on our
# fork, identical results to stock; docs/rcheevos-pin.md). On by default; DSC_FLAGS= turns them off.
DSC_FLAGS ?= -DDSC_CHANGE_DRIVEN -DDSC_CHANGED_RANGES
CFLAGS   += $(DSC_FLAGS)
# header dependencies (build/*.d): a changed header rebuilds every object that includes it
CFLAGS   += -MMD -MP $(EXTRA_CFLAGS)
# The module's data (statics, the snapshot arena, the session, small sets' heap) through the
# 0x0C000000 mirror, which most games' MPUs leave uncached: our data never evicts the game's
# cache lines (tests/perf/AB.md "Data through the mirror"). DATA_MIRROR=0: the cached layout.
DATA_MIRROR ?= 1
ifeq ($(DATA_MIRROR),1)
CFLAGS   += -DDSCM_DATA_MIRROR
# the core's views line (.data.cached) at its fixed address, module.ld asserts it;
# a CACHED_COLOUR sweep leaves it to the linker
ifeq ($(CACHED_COLOUR),)
CFLAGS   += -DDSC_VIEWS_ADDR=0x02D80540u
endif
endif
# mbedTLS's calloc/free set at run time (src/net/https.c: through the cache)
CFLAGS   += -DMBEDTLS_PLATFORM_MEMORY
# heap.c places load-time memory (heap.h): it wraps malloc and rc_api's parse of the set
# fast_strtoul.c: newlib's strtoul without its per-call divisions
WRAPS    := malloc free calloc realloc rc_api_process_fetch_game_sets_server_response strtoul
comma    := ,
# the idle stack's top, this far below the module region's end (start.s, module.ld)
STACK_GAP ?= 0x280
LDFLAGS  := -Wl$(comma)--defsym$(comma)DSCM_STACK_GAP=$(STACK_GAP) $(if $(CACHED_COLOUR),-Wl$(comma)--defsym$(comma)DSCM_CACHED_COLOUR=$(CACHED_COLOUR)) $(if $(WAKE_COLOUR),-Wl$(comma)--defsym$(comma)DSCM_WAKE_COLOUR=$(WAKE_COLOUR)) $(if $(TEXT_COLOUR),-Wl$(comma)--defsym$(comma)DSCM_TEXT_COLOUR=$(TEXT_COLOUR)) \
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

# the hot-layout check (below) runs for the default build only: no colour, script or flag overrides
# LAB coverage: BW=1 builds the BIOS-wait gate with preemptible evaluation (research/optimize/biowait.md):
# the fiber, rcheevos' yield points, the TLS yield points, the idle path's run-time ownership
ifeq ($(BW),1)
TLS_YIELD := 1
BW_ASFLAGS := -Wa,--defsym,BW_FIBER=1 -Wa,--defsym,TLS_YIELD=1
RC_YIELD ?= dscm_yield_point
CFLAGS += -DBW_FIBER -DBW_HALT_CAPTURE -DTLS_YIELD -DDSC_IDLE_MAX $(if $(RC_YIELD),-DRC_CLIENT_YIELD_HOOK=$(RC_YIELD)) -DDSC_ROOM_HOOK=dscm_room
HOTCHECK := 0
endif
# BW_PROTO=1: round 1's read-only gate prototype (no fiber)
ifeq ($(BW_PROTO),1)
CFLAGS += -DBW_PROTO -DDSC_IDLE_MAX
HOTCHECK := 0
endif
HOTCHECK ?= $(if $(CACHED_COLOUR)$(WAKE_COLOUR)$(TEXT_COLOUR)$(DATA_COLOUR)$(LDSCRIPT)$(EXTRA_CFLAGS)$(EXTRA_LDFLAGS)$(filter-out file,$(origin DSC_FLAGS))$(filter-out 1,$(DATA_MIRROR))$(filter command line,$(origin STACK_GAP)),0,1)
.PHONY: all clean hotcheck hot-accept
all: $(BUILD)/dscheevos9.bin $(if $(filter 1,$(HOTCHECK)),hotcheck)

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
# LAB coverage (TLS_YIELD=1, BW_FIBER builds): bignum_core.o's own calls to these reach module.c's
# yield-point wrappers (the symbol weakened, its code kept as dsc_<f>_real)
TLS_YIELD_CORE := montmul lt_ct
# and these calls between mbedtls files go through start.s's yield trampolines (--wrap)
TLS_YIELD_WRAPS := mbedtls_mpi_mul_mpi mbedtls_mpi_mod_mpi mbedtls_mpi_copy mbedtls_mpi_sub_mpi mbedtls_mpi_add_mpi mbedtls_mpi_shift_r mbedtls_mpi_cmp_mpi mbedtls_mpi_read_binary mbedtls_mpi_write_binary mbedtls_mpi_random mbedtls_mpi_exp_mod mbedtls_md_update mbedtls_md_finish mbedtls_md_hmac_update mbedtls_md_hmac_starts mbedtls_sha256_update mbedtls_asn1_get_tag mbedtls_ssl_read_record mbedtls_cipher_setkey mbedtls_x509_crt_parse_der_nocopy mbedtls_ecp_group_load mbedtls_pk_parse_subpubkey mbedtls_cipher_update mbedtls_aes_crypt_ecb mbedtls_chacha20_update mbedtls_cipher_auth_decrypt_ext mbedtls_cipher_auth_encrypt_ext mbedtls_ecdh_calc_secret mbedtls_ecdh_make_public
LDFLAGS += $(if $(TLS_YIELD),$(foreach w,$(TLS_YIELD_WRAPS),-Wl$(comma)--wrap=$(w)))
$(BUILD)/tls/%.o: $(MBEDTLS)/library/%.c $(TLSCONF)/dsc_mbedtls_config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter bignum% ecp% ecdsa ecdh,$*),-marm -O2) -w -c $< -o $@
	$(if $(and $(TLS_YIELD),$(filter bignum_core,$*)),$(OBJCOPY) $(foreach f,$(TLS_YIELD_CORE),--add-symbol dsc_$(f)_real=.text.mbedtls_mpi_core_$(f):0$(comma)global$(comma)function --weaken-symbol=mbedtls_mpi_core_$(f)) $@)
$(BUILD)/net/%.o: src/net/%.c src/net/net.h include/dscheevos_module.h include/dscm_shim.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/net/cacerts.o: $(TLSCONF)/cacerts.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/start.o: src/start.s
	@mkdir -p $(BUILD)
	$(CC) $(ARCH) -Wa,--defsym,STACK_GAP=$(STACK_GAP) $(BW_ASFLAGS) $(EXTRA_ASFLAGS) -c $< -o $@

$(BUILD)/dscheevos9.elf: $(OBJ) module.ld module-cached.ld hot.ld warm.ld hot-rodata.ld
	$(CC) $(LDFLAGS) $(OBJ) -lc -lm -lgcc -o $@
	$(SIZE) $@

$(BUILD)/dscheevos9.bin: $(BUILD)/dscheevos9.elf
	$(OBJCOPY) -O binary $< $@
	@echo "built $@ ($$(stat -c %s $@) bytes)"

# Layout firewall (module.ld, research/optimize/layout.md): the hot code's and hot constants'
# addresses (the wake block, hot.ld, hot-rodata.ld) must be those in hot-layout.ref. A change
# to cold or load-time code can't move them; a change that does (hot code, hot.ld, a toolchain
# or submodule bump) fails here: measure the game cost (tests/perf/gamecost.sh), then
# `make hot-accept`. Checked for the default build only (HOTCHECK, set above).
$(BUILD)/hot-layout.txt: $(BUILD)/dscheevos9.elf
	$(NM) -n $< > $@.nm
	awk 'NR==FNR { if ($$3=="__hot_end") he=$$1; if ($$3=="__hot_rodata_start") rs=$$1; if ($$3=="__hot_rodata_end") re=$$1; next } \
	  NF==3 && $$3 !~ /^__hot/ && (($$1>="02d80000" && $$1<he) || ($$1>=rs && $$1<re)) { print $$1, $$3 }' $@.nm $@.nm > $@
	@rm -f $@.nm
hotcheck: $(BUILD)/hot-layout.txt
	@if diff -u hot-layout.ref $< > $(BUILD)/hot-layout.diff; then echo "hotcheck: hot layout unchanged ($$(wc -l < $<) symbols)"; \
	else head -40 $(BUILD)/hot-layout.diff; echo "hotcheck: FAIL: the hot code or constants moved (diff above, $(BUILD)/hot-layout.diff)."; \
	  echo "  If the change is meant to touch hot code: measure the game cost, then make -C dscheevos/module hot-accept"; exit 1; fi
hot-accept: $(BUILD)/hot-layout.txt
	cp $< hot-layout.ref

-include $(OBJ:.o=.d)

clean:
	rm -rf $(BUILD)
