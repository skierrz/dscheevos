# Host build of the DScheevos display library and its tests.
#   make test                 build and run the tests (compares golden PNGs)
#   make test UPDATE_GOLDEN=1 rewrite the golden PNGs (review them by eye first)
BOOTSTRAP ?= ../../external/nds-bootstrap
CC        ?= cc
CFLAGS    ?= -O1 -g -Wall -Wextra
PYTHON    ?= python3
INCLUDES  := -Iinclude -I../module/include

BUILD := build
SRC   := src/dsd_sched.c src/dsd_render.c src/dsd_menu.c
HDR   := include/dsd.h ../module/include/dscheevos_module.h

.PHONY: test clean
test: $(BUILD)/test_display
	cd tests && UPDATE_GOLDEN=$(UPDATE_GOLDEN) ../$(BUILD)/test_display ../$(BUILD)/out

# The in-game menu's font, built from the nds-bootstrap checkout (not copied here).
$(BUILD)/font_latin.c: tools/fontgen.py
	@mkdir -p $(BUILD)
	$(PYTHON) tools/fontgen.py $(BOOTSTRAP) extended_latin dsd_font_latin $@

$(BUILD)/test_display: tests/test_display.c tests/png.h $(SRC) $(HDR) $(BUILD)/font_latin.c
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_display.c $(SRC) $(BUILD)/font_latin.c -o $@

clean:
	rm -rf $(BUILD)
