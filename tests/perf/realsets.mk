# Read together with dscheevos/module/Makefile (as perf.mk), so the core and
# rcheevos are compiled by the module's own rules and flags:
#   make -C dscheevos/module -f Makefile -f ../../tests/perf/realsets.mk \
#     BUILD=<abs dir> [EXTRA_CFLAGS=...] <abs dir>/realsets.elf
# EXTRA_CFLAGS is added to every compile (e.g. the float-operand variant).
REAL_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
REAL_OBJ := $(BUILD)/dscheevos.o $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/rc/%.o,$(RC_SRC))
CFLAGS += $(EXTRA_CFLAGS)

$(BUILD)/realsets.o: $(REAL_DIR)realsets.c $(CORE)/include/dscheevos.h include/dscheevos_module.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/realsets.elf: $(BUILD)/realsets.o $(REAL_OBJ)
	$(CC) $(ARCH) -specs=rdimon.specs -Wl,--gc-sections -Wl,--wrap=rc_client_do_frame $^ -lm -o $@
