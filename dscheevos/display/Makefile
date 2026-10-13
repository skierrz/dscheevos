# Host build of the DScheevos display library and its tests.
#   make test                 build and run the tests (compares golden PNGs)
#   make test BADGES=1        also the badge pipeline (launcher side, not yet in the launcher)
#   make test UPDATE_GOLDEN=1 rewrite the golden PNGs (review them by eye first)
#   DSD_BADGE_SAMPLES=<dir>   with BADGES=1: convert the PNGs there into build/out/samples.png
BOOTSTRAP ?= ../../external/nds-bootstrap
CC        ?= cc
CFLAGS    ?= -O1 -g -Wall -Wextra
PYTHON    ?= python3
INCLUDES  := -Iinclude -I../module/include

BUILD := build
SRC   := src/dsd_sched.c src/dsd_render.c src/dsd_menu.c src/dsd_badge.c src/dsd_strip.c
HDR   := include/dsd.h include/dsd_badge.h include/dsd_strip.h ../module/include/dscheevos_module.h

.PHONY: test clean
test: $(BUILD)/test_display $(BUILD)/test_strip $(if $(BADGES),$(BUILD)/test_badges)
	cd tests && UPDATE_GOLDEN=$(UPDATE_GOLDEN) ../$(BUILD)/test_display ../$(BUILD)/out
	$(BUILD)/test_strip
ifneq ($(BADGES),)
	cd tests && UPDATE_GOLDEN=$(UPDATE_GOLDEN) ../$(BUILD)/test_badges ../$(BUILD)/out
endif

# The in-game menu's font, built from the nds-bootstrap checkout (not copied here).
$(BUILD)/font_latin.c: tools/fontgen.py
	@mkdir -p $(BUILD)
	$(PYTHON) tools/fontgen.py $(BOOTSTRAP) extended_latin dsd_font_latin $@

$(BUILD)/test_display: tests/test_display.c tests/png.h $(SRC) $(HDR) $(BUILD)/font_latin.c
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_display.c $(SRC) $(BUILD)/font_latin.c -o $@

$(BUILD)/test_strip: tests/test_strip.c src/dsd_strip.c include/dsd_strip.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_strip.c src/dsd_strip.c -o $@

BADGE_SRC := badgepack/dsd_badgepack.c
$(BUILD)/test_badges: tests/test_badges.c tests/png.h $(BADGE_SRC) badgepack/dsd_badgepack.h third_party/stb_image.h $(SRC) $(HDR) $(BUILD)/font_latin.c
	$(CC) $(CFLAGS) $(INCLUDES) -Ibadgepack tests/test_badges.c $(BADGE_SRC) $(SRC) $(BUILD)/font_latin.c -o $@

clean:
	rm -rf $(BUILD)
