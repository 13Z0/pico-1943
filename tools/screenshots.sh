#!/usr/bin/env bash
# Make the README's screenshots, docs/screenshots/*.png, from your own ROM
# sets (the sets are not part of this project, so neither are pictures of the
# game until you make them):
#
#   tools/screenshots.sh <romroot> [workdir]
#
# <romroot> holds the folders 1943 and 1943KAI as on the SD card. The host
# harness plays both sets with scripted input; the upright pictures are taken
# as an HSTX board shows them (--tall, 640x480), the tate ones from the
# 320x240 canvas, doubled. The frames used are kept in workdir (default
# hosttest/out/screenshots).
set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 1 ]; then
    sed -n 2,13p "$0"
    exit 1
fi
roms=$1
work=${2:-hosttest/out/screenshots}
mkdir -p "$work" docs/screenshots

[ -x hosttest/c1943_host ] || hosttest/build.sh
run() { ./hosttest/c1943_host "$roms" "$@" > /dev/null; }
run 1943 2401 600 "$work/up" --tall --coin 1200 --start 1260 --play 1320
run 1943 2401 2400 "$work/cw" --tate 1 --coin 1200 --start 1260 --play 1320
run 1943 2401 2400 "$work/ccw" --tate 2 --coin 1200 --start 1260 --play 1320
run 1943kai 1201 1200 "$work/kai" --tall

shot() { python3 hosttest/ppm2png.py ${3:-} "$work/$1" > /dev/null; cp "${work}/${1%.ppm}.png" "docs/screenshots/$2.png"; }
shot up/frame_00600.ppm title
shot up/frame_01800.ppm briefing
shot up/frame_02400.ppm battle
shot kai/frame_01200.ppm kai-title
shot cw/frame_02400.ppm tate-bottom-left --x2
shot ccw/frame_02400.ppm tate-bottom-right --x2
ls -l docs/screenshots
