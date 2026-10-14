#!/bin/sh
# Builds the copy-once replay harness on the host. RCHEEVOS=dir of the rcheevos checkout.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
RC=${RCHEEVOS:-$here/../../external/rcheevos}
out=${1:-$here/../../build/co}
mkdir -p "$out"
set --
for s in client/rc_client.c runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c \
  runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c runtime/rc_richpresence.c \
  runtime/rc_runtime.c runtime/rc_runtime_progress.c runtime/rc_trigger.c runtime/rc_value.c api/rc_api_common.c \
  api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c; do
  set -- "$@" "$RC/src/$s"
done
cc -O2 -DRC_NO_THREADS -DNDEBUG -Wno-unused-result -I"$RC/include" -I"$RC/src" "$here/copyonce.c" "$@" -lm -o "$out/copyonce"
