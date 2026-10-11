#!/bin/bash
# $1=elf, rest=args. Prints per-phase instruction counts and a PC histogram for phase 3 (do_frame).
ELF=$1; shift
MB=$(arm-none-eabi-nm $ELF | awk '$3=="mark_begin"{print $1}'); ME=$(arm-none-eabi-nm $ELF | awk '$3=="mark_end"{print $1}')
qemu-arm -cpu arm946 -one-insn-per-tb -d exec,nochain -D /dev/stdout $ELF "$@" 2>/dev/null | awk -v mb=$MB -v me=$ME '
/^Trace/ { split($4,a,"/"); pc=a[2]; sub(/^0+/,"",pc);
  if (pc==sub_mb) {}
  if (pc==b) { inph=1; n=0; ph++; next }
  if (pc==e) { inph=0; cnt[ph]=n; next }
  if (inph) { n++; if (ph>2) hist[pc]++ } next }
BEGIN { b=mb; sub(/^0+/,"",b); e=me; sub(/^0+/,"",e) }
END { for (i=1;i<=ph;i++) print "PHASE", i, cnt[i]; for (p in hist) print "PC", p, hist[p] }'
