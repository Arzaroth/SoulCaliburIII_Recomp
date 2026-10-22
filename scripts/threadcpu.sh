#!/bin/bash
PID=$1; DUR=$2
declare -A a
for t in /proc/$PID/task/*; do id=${t##*/}; s=($(cut -d')' -f2 $t/stat)); a[$id]=$(( ${s[11]} + ${s[12]} )); done
sleep $DUR
for t in /proc/$PID/task/*; do id=${t##*/}; s=($(cut -d')' -f2 $t/stat)); n=$(( ${s[11]} + ${s[12]} )); d=$(( n - ${a[$id]:-0} )); [ $d -gt 5 ] && echo "$(cat $t/comm) $id $(( d * 100 / (DUR * 100) ))%"; done
