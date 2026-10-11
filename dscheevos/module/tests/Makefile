# Host tests for the module's network code.
#   make test        build and run them
MBEDTLS  ?= ../../../external/mbedtls
TLSCONF  := ../../launcher/tls
CC       ?= cc
CFLAGS   ?= -O1 -g -Wall -Wextra
INCLUDES := -I../src/net -I$(TLSCONF) -I$(MBEDTLS)/include '-DMBEDTLS_CONFIG_FILE="dsc_mbedtls_config.h"'

BUILD   := build
TLS_SRC := $(wildcard $(MBEDTLS)/library/*.c)
TLS_OBJ := $(patsubst $(MBEDTLS)/library/%.c,$(BUILD)/tls/%.o,$(TLS_SRC))

.PHONY: test clean
test: $(BUILD)/test_wpa
	$(BUILD)/test_wpa

$(BUILD)/tls/%.o: $(MBEDTLS)/library/%.c $(TLSCONF)/dsc_mbedtls_config.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -w $(INCLUDES) -c $< -o $@

$(BUILD)/libtls.a: $(TLS_OBJ)
	ar rcs $@ $^

$(BUILD)/test_wpa: test_wpa.c ../src/net/wpa.c ../src/net/wpa.h $(BUILD)/libtls.a
	$(CC) $(CFLAGS) $(INCLUDES) test_wpa.c ../src/net/wpa.c $(BUILD)/libtls.a -o $@

clean:
	rm -rf $(BUILD)
