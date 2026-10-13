# Host tests for the launcher's plain-C parts: make -C dscheevos/launcher/tests test
CC ?= cc
CFLAGS += -std=c99 -Wall -Wextra -Werror -I../source -I../../module/include

test: test_clock
	./test_clock

test_clock: test_clock.c ../source/clock.c ../source/clock.h ../../module/include/dscheevos_module.h
	$(CC) $(CFLAGS) -o $@ test_clock.c ../source/clock.c

clean:
	rm -f test_clock

.PHONY: test clean
