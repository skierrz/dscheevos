# tests/difftest: the rcheevos differential harness (docs/rcheevos-difftest.md).
#   make                      difftest-run-stock, difftest-run-patched (host)
#   STOCK_REF=683197d         the pinned upstream commit (the reference)
#   PATCHED=../../external/rcheevos   our fork's tree (the candidate)
# Build products go to build/ (untracked).
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
RC_DEFS  := -DRC_NO_THREADS
STOCK_REF ?= 683197d
PATCHED  ?= ../../external/rcheevos
# the change-driven patch, if the patched tree has it
CD_DEF   := $(shell grep -q rc_client_set_change_driven_enabled $(PATCHED)/include/rc_client.h 2>/dev/null && echo -DDIFFTEST_CHANGE_DRIVEN)
# the changed-memory patch, if the patched tree has it
CHG_DEF  := $(shell grep -q rc_client_set_changed_memory $(PATCHED)/include/rc_client.h 2>/dev/null && echo -DDIFFTEST_CHANGES)
# the partition patch, if the patched tree has it
PART_DEF := $(shell grep -q rc_client_set_trigger_evaluator $(PATCHED)/include/rc_client.h 2>/dev/null && echo -DDIFFTEST_PARTITION)
BUILD    := build
RC_FILES := client/rc_client.c \
  runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c \
  runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c \
  runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c \
  runtime/rc_trigger.c runtime/rc_value.c \
  api/rc_api_common.c api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c \
  util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c

.PHONY: all clean core
all: $(BUILD)/difftest-run-stock $(BUILD)/difftest-run-patched $(BUILD)/difftest-run-patched-tsan
core: $(BUILD)/difftest-run-core $(BUILD)/difftest-run-core-noflags

# core mode: the DScheevos core (dscheevos/core) runs the patched rc_client, with the module's flags
# (CORE_FLAGS) or without; --handover SEED adds the two simulated evaluators
CORE      := ../../dscheevos/core
CORE_FLAGS ?= -DDSC_CHANGE_DRIVEN -DDSC_CHANGED_RANGES
$(BUILD)/difftest-run-core: run.c sha256.h $(CORE)/src/dscheevos.c $(CORE)/include/dscheevos.h $(addprefix $(PATCHED)/src/,$(RC_FILES))
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-missing-field-initializers $(RC_DEFS) -DDIFFTEST_CORE -DDSC_TEST_HOOKS -DDSC_CHECK_FAST_STEPS $(CORE_FLAGS) \
	  -I$(CORE)/include -I$(PATCHED)/include -I$(PATCHED)/src run.c $(CORE)/src/dscheevos.c \
	  $(addprefix $(PATCHED)/src/,$(RC_FILES)) -lm -o $@
$(BUILD)/difftest-run-core-noflags: run.c sha256.h $(CORE)/src/dscheevos.c $(CORE)/include/dscheevos.h $(addprefix $(PATCHED)/src/,$(RC_FILES))
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-missing-field-initializers $(RC_DEFS) -DDIFFTEST_CORE -DDSC_TEST_HOOKS -DDSC_CHECK_FAST_STEPS \
	  -I$(CORE)/include -I$(PATCHED)/include -I$(PATCHED)/src run.c $(CORE)/src/dscheevos.c \
	  $(addprefix $(PATCHED)/src/,$(RC_FILES)) -lm -o $@

$(BUILD)/stock/.ok:
	@mkdir -p $(BUILD)/stock
	git -C $(PATCHED) archive $(STOCK_REF) src include | tar -x -C $(BUILD)/stock
	touch $@

$(BUILD)/difftest-run-stock: run.c sha256.h $(BUILD)/stock/.ok
	$(CC) $(CFLAGS) $(RC_DEFS) -I$(BUILD)/stock/include -I$(BUILD)/stock/src run.c \
	  $(addprefix $(BUILD)/stock/src/,$(RC_FILES)) -lm -o $@

$(BUILD)/difftest-run-patched: run.c sha256.h $(addprefix $(PATCHED)/src/,$(RC_FILES)) $(PATCHED)/include/rc_client.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(RC_DEFS) $(CD_DEF) $(CHG_DEF) $(PART_DEF) -DDIFFTEST_THREADS -I$(PATCHED)/include -I$(PATCHED)/src run.c \
	  $(addprefix $(PATCHED)/src/,$(RC_FILES)) -lm -lpthread -o $@

# the threaded partition run under ThreadSanitizer
$(BUILD)/difftest-run-patched-tsan: run.c sha256.h $(addprefix $(PATCHED)/src/,$(RC_FILES)) $(PATCHED)/include/rc_client.h
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -fsanitize=thread $(RC_DEFS) $(CD_DEF) $(CHG_DEF) $(PART_DEF) -DDIFFTEST_THREADS -I$(PATCHED)/include -I$(PATCHED)/src run.c \
	  $(addprefix $(PATCHED)/src/,$(RC_FILES)) -lm -lpthread -o $@

# measurement builds: copies of both trees with condition counters (stats.sh), --stats FILE
$(BUILD)/difftest-stats-stock: run.c sha256.h $(BUILD)/stock/.ok
	./mkstats.sh $(BUILD)/stock $(BUILD)/stats-stock
	$(CC) $(CFLAGS) $(RC_DEFS) -DDIFFTEST_STATS -I$(BUILD)/stats-stock/include -I$(BUILD)/stats-stock/src run.c \
	  $(addprefix $(BUILD)/stats-stock/src/,$(RC_FILES)) -lm -o $@

.PHONY: $(BUILD)/difftest-stats-patched
$(BUILD)/difftest-stats-patched: run.c sha256.h
	./mkstats.sh $(PATCHED) $(BUILD)/stats-patched
	$(CC) $(CFLAGS) $(RC_DEFS) $(CD_DEF) $(CHG_DEF) -DDIFFTEST_STATS -I$(BUILD)/stats-patched/include -I$(BUILD)/stats-patched/src run.c \
	  $(addprefix $(BUILD)/stats-patched/src/,$(RC_FILES)) -lm -o $@

clean:
	rm -rf $(BUILD)
