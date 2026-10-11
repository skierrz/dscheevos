#!/bin/bash
# usage: build.sh <outdir> <extra defines...>
set -e
RC=${RC:?set RC to an rcheevos checkout}; OUT=$1; shift; DEFS="$*"
CFLAGS="-mcpu=arm946e-s -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -fstack-usage -fcallgraph-info=su -g -std=gnu99 -Wall -fno-signed-char -I$RC/include $DEFS"
rm -rf $OUT; mkdir -p $OUT
declare -A MODS=(
 [client]="client/rc_client.c client/rc_client_external.c client/rc_client_raintegration.c"
 [runtime]="runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c runtime/rc_trigger.c runtime/rc_value.c runtime/rc_validate.c"
 [api]="api/rc_api_common.c api/rc_api_editor.c api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c"
 [hash]="hash/rc_hash.c hash/rc_hash_rom.c hash/rc_hash_disc.c hash/rc_cdreader.c hash/rc_hash_zip.c hash/rc_hash_encrypted.c hash/aes.c"
 [util]="util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c"
)
for m in "${!MODS[@]}"; do
  mkdir -p $OUT/$m
  for f in ${MODS[$m]}; do
    o=$OUT/$m/$(basename ${f%.c}).o
    MF=""; case " $ARM_MODS " in *" $m "*) MF="-marm";; esac
    (cd $OUT/$m && arm-none-eabi-gcc $CFLAGS $MF -c $RC/src/$f -o $o) 2>>$OUT/warnings.txt
  done
  # one relocatable object per module, as asked
  arm-none-eabi-ld -r -o $OUT/rc_$m.o $OUT/$m/*.o
  arm-none-eabi-ar rcs $OUT/librc_$m.a $OUT/$m/*.o
done
arm-none-eabi-gcc $CFLAGS -c "$(dirname "$0")/driver.c" -o $OUT/driver.o
# libretro deliberately left out of the link: DScheevos is not a libretro core
arm-none-eabi-gcc -mcpu=arm946e-s -mthumb -specs=nosys.specs -Wl,--gc-sections -Wl,-Map=$OUT/link.map \
  -Wl,--print-gc-sections $OUT/driver.o -Wl,--start-group $OUT/librc_client.a $OUT/librc_runtime.a $OUT/librc_api.a $OUT/librc_hash.a $OUT/librc_util.a -Wl,--end-group -lm \
  -o $OUT/dscheevos_probe.elf 2>$OUT/gc.txt
echo built $OUT
