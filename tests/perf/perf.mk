# Read together with dscheevos/module/Makefile, so the core and rcheevos are
# compiled by the module's own rules and flags (Thumb or ARM, -Os...):
#   make -C dscheevos/module -f Makefile -f ../../tests/perf/perf.mk \
#     BUILD=<abs dir> PERF_SET=typical <abs dir>/perf-typical.elf
# expects <abs dir>/set-typical.h (tools/rcheevos-footprint/harness/gen_set.py).
PERF_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
PERF_OBJ := $(BUILD)/dscheevos.o $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/rc/%.o,$(RC_SRC))

$(BUILD)/perf-$(PERF_SET).o: $(PERF_DIR)perf.c $(BUILD)/set-$(PERF_SET).h $(CORE)/include/dscheevos.h include/dscheevos_module.h
	$(CC) $(CFLAGS) '-DSET_HEADER="$(BUILD)/set-$(PERF_SET).h"' -c $< -o $@

$(BUILD)/perf-$(PERF_SET).elf: $(BUILD)/perf-$(PERF_SET).o $(PERF_OBJ)
	$(CC) $(ARCH) -specs=rdimon.specs -Wl,--gc-sections $^ -lm -o $@
