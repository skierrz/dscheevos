# Read together with dscheevos/module/Makefile (as realsets.mk), so the core,
# rcheevos and heap.c are compiled by the module's own rules and flags:
#   make -C dscheevos/module -f Makefile -f ../../tests/perf/fitmem.mk \
#     BUILD=<abs dir> <abs dir>/fitmem.elf
FIT_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
FIT_OBJ := $(BUILD)/dscheevos.o $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/rc/%.o,$(RC_SRC))
FIT_WRAPS := malloc free calloc realloc rc_api_process_fetch_game_sets_server_response \
  rc_api_init_start_session_request_hosted rc_api_destroy_fetch_game_sets_response rc_client_do_frame

$(BUILD)/fitmem-heap.o: src/heap.c src/heap.h include/dscheevos_module.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DDSCM_HEAP_HOST_TEST -c $< -o $@

$(BUILD)/fitmem.o: $(FIT_DIR)fitmem.c $(CORE)/include/dscheevos.h include/dscheevos_module.h src/heap.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fitmem.elf: $(BUILD)/fitmem.o $(BUILD)/fitmem-heap.o $(FIT_OBJ)
	$(CC) $(ARCH) -specs=rdimon.specs -Wl,--gc-sections $(foreach w,$(FIT_WRAPS),-Wl,--wrap=$(w)) $^ -lm -o $@
