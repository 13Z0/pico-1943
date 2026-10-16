#!/usr/bin/env bash
# Takes the pictures tools/mkartwork.py lays out from your own ROM sets (the
# sets are not part of this project, so neither are pictures of the game):
#
#   tools/capture_footage.sh <romroot> <outdir>
#
# <romroot> holds the folders 1943 and 1943KAI as on the SD card. The host
# harness plays both sets with the input of tools/screenshots.sh; each picture
# is a 320x240 PPM with Tate mode Off, the game at x 48-271. To change a part
# of the artwork, copy another frame over its name.
set -euo pipefail
cd "$(dirname "$0")/.."

[ $# -eq 2 ] || { sed -n 2,10p "$0"; exit 1; }
roms=$1
out=$2
work="$out/frames"
mkdir -p "$work"

[ -x hosttest/c1943_host ] || hosttest/build.sh
run() { ./hosttest/c1943_host "$roms" "$@" > /dev/null; }
run 1943 2401 300 "$work/1943" --coin 1200 --start 1260 --play 1320
run 1943kai 3301 300 "$work/kai" --coin 1200 --start 1260 --play 1320

grab() { cp "$work/$1" "$out/$2.ppm"; echo "$1 -> $out/$2.ppm"; }
grab 1943/frame_00300.ppm title       # theme 0 badge: the 1943 logo
grab kai/frame_00300.ppm kai-title    # theme 0 badge: the 1943 Kai logo
grab 1943/frame_02400.ppm left        # theme 1: a formation of fighters
grab kai/frame_01500.ppm middle       # theme 1: the carrier and its bombers
grab kai/frame_03300.ppm right        # theme 1: an island
