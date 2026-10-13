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
BUILD    := build
RC_FILES := client/rc_client.c \
  runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c \
  runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c \
  runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c \
  runtime/rc_trigger.c runtime/rc_value.c \
  api/rc_api_common.c api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c \
  util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c

.PHONY: all clean
all: $(BUILD)/difftest-run-stock $(BUILD)/difftest-run-patched $(BUILD)/difftest-run-patched-tsan

$(BUILD)/stock/.ok:
	@mkdir -p $(BUILD)/stock
	git -C $(PATCHED) archive $(STOCK_REF) src include | tar -x -C $(BUILD)/stock
	touch $@

$(BUILD)/difftest-run-stock: run.c sha256.h $(BUILD)/stock/.ok
	$(CC) $(CFLAGS) $(RC_DEFS) -I$(BUILD)/stock/include -I$(BUILD)/stock/src run.c \
	  $(addprefix $(BUILD)/stock/src/,$(RC_FILES)) -lm -o $@

$(BUILD)/difftest-run-patched: run.c sha256.h $(addprefix $(PATCHED)/src/,$(RC_FILES)) $(PATCHED)/include/rc_client.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(RC_DEFS) -DDIFFTEST_PATCHED -DDIFFTEST_THREADS -I$(PATCHED)/include -I$(PATCHED)/src run.c \
	  $(addprefix $(PATCHED)/src/,$(RC_FILES)) -lm -lpthread -o $@

# the threaded partition run under ThreadSanitizer
$(BUILD)/difftest-run-patched-tsan: run.c sha256.h $(addprefix $(PATCHED)/src/,$(RC_FILES)) $(PATCHED)/include/rc_client.h
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -fsanitize=thread $(RC_DEFS) -DDIFFTEST_PATCHED -DDIFFTEST_THREADS -I$(PATCHED)/include -I$(PATCHED)/src run.c \
	  $(addprefix $(PATCHED)/src/,$(RC_FILES)) -lm -lpthread -o $@

clean:
	rm -rf $(BUILD)
