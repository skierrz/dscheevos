#!/bin/bash
# $1 = trace output from count.sh
grep PHASE $1 | awk '$2==1{print "login (create+login2 parse):",$3} $2==2{print "load_game (parse JSON + activate):",$3}'
grep PHASE $1 | awk 'NR>2' | sed '$d' | awk '{print $3}' | sort -n | awk '{a[NR]=$1; s+=$1} END{printf "do_frame x%d: min %d  median %d  p95 %d  max %d  mean %d\n",NR,a[1],a[int(NR/2)+1],a[int(NR*0.95)],a[NR],s/NR}'
grep PHASE $1 | tail -1 | awk '{print "get_rich_presence_message:",$3}'
