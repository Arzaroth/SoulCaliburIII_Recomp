#!/usr/bin/env bash
# Extracts the files of your own Soulcalibur III (NTSC-U, SLUS-21216) disc image into game/.
# Usage: scripts/extract_iso.sh <image.iso>
source "$(dirname "$0")/common.sh"
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 <image.iso>"; exit 1; }
command -v bsdtar >/dev/null || { echo "bsdtar not found (libarchive)"; exit 1; }
mkdir -p "$GAME_DIR"
bsdtar -xf "$1" -C "$GAME_DIR"
chmod -R u+w "$GAME_DIR"
[ -f "$ELF" ] || { echo "no SLUS_212.16 in $1: not the NTSC-U disc?"; exit 1; }
expected=9bbb67a70137052931bc245855eedeee6bbe5b49
actual=$(sha1sum "$ELF" | cut -d' ' -f1)
[ "$actual" = "$expected" ] || echo "warning: SLUS_212.16 SHA-1 is $actual, the port is built against $expected"
echo "Disc files in $GAME_DIR"
