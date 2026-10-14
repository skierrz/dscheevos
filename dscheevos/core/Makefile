# Host build of the DScheevos core and its unit tests.
#   make test        build and run the tests
#   make test DSC_FLAGS="-DDSC_CHANGE_DRIVEN -DDSC_CHANGED_RANGES" BUILD=build/flags
#                    the same with the module's default flags
RCHEEVOS ?= ../../external/rcheevos
CC       ?= cc
CFLAGS   ?= -O1 -g -Wall -Wextra
DEFS     := -DRC_NO_THREADS
DSC_FLAGS ?=
INCLUDES := -Iinclude -I$(RCHEEVOS)/include -I$(RCHEEVOS)/src

RC_SRC := $(addprefix $(RCHEEVOS)/src/, \
  client/rc_client.c \
  runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c \
  runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c \
  runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c \
  runtime/rc_trigger.c runtime/rc_value.c \
  api/rc_api_common.c api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c \
  util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c)

BUILD := build
RC_OBJ := $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/rc/%.o,$(RC_SRC))

.PHONY: test clean
test: $(BUILD)/test_core
	$(BUILD)/test_core

$(BUILD)/rc/%.o: $(RCHEEVOS)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(DEFS) $(INCLUDES) -c $< -o $@

$(BUILD)/test_core: tests/test_core.c src/dscheevos.c include/dscheevos.h $(RC_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(DSC_FLAGS) -DDSC_CHECK_FAST_STEPS $(INCLUDES) tests/test_core.c src/dscheevos.c $(RC_OBJ) -lm -o $@

clean:
	rm -rf $(BUILD)
