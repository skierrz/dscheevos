#!/bin/bash
# build harness against a given rcheevos build dir; $1=builddir $2=set header $3=out name
set -e; B=$1; H=$2; O=$3; RC=${RC:?set RC to an rcheevos checkout}
arm-none-eabi-gcc -mcpu=arm946e-s -mthumb -Os -ffunction-sections -fdata-sections -std=gnu99 -I$RC/include -DSET_HEADER="\"$H\"" -c harness.c -o $O.o
arm-none-eabi-gcc -mcpu=arm946e-s -mthumb -specs=rdimon.specs -Wl,--gc-sections \
  -Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc $O.o \
  -Wl,--start-group $B/librc_client.a $B/librc_runtime.a $B/librc_api.a $B/librc_hash.a $B/librc_util.a -Wl,--end-group -lm -o $O.elf
