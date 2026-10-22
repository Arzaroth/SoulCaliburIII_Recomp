#!/bin/bash
# usage: dis.sh <start> <stop>  - disassemble the game ELF (SC3_ELF) between two addresses
llvm-objdump -d --no-show-raw-insn --triple=mipsel --mcpu=mips3 --start-address=$1 --stop-address=$2 "${SC3_ELF:-$HOME/Games/PS2/SoulcaliburIII/disc/SLUS_212.16}" | grep -E '^ +[0-9a-f]+:' | sed -E 's/<\.text\+0x[0-9a-f]+>//'
