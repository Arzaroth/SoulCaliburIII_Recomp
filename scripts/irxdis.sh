#!/bin/bash
# irxdis.sh <irx> <start offset> <end offset>
llvm-objdump -d --no-show-raw-insn --triple=mipsel --mcpu=mips2 -r --start-address=$2 --stop-address=$3 "$1" | grep -E '^\s+[0-9a-f]+:|R_MIPS' | sed -E 's/<\.text[^>]*>//' | tr -s ' \t' ' '
