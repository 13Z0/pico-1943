#!/usr/bin/env bash
# Build the host harnesses:
#   hosttest/c1943_host  - the 1943 core (main CPU, sound board, video, ROM loader)
#   hosttest/zex_host    - the Z80 core on the ZEXDOC/ZEXALL instruction tests
#   hosttest/ym_compare  - ymfm's rendering of c1943_host --ymlog, for checking the
#                          sound (only when ymfm's sources are found: YMFM_DIR, by
#                          default ares' copy next to this repository)
#
# The core in c1943/ has no Pico dependencies; this compiles it for Linux with
# ASan. Drop -fsanitize=address (and use -O2) for timing measurements:
#   OPT="-O2" hosttest/build.sh
# With SDL2 installed (libsdl2-dev), c1943_host also gets live mode (--live,
# hosttest/host_sdl.c): a window, sound and the keyboard.
set -euo pipefail
cd "$(dirname "$0")/.."

SDL=()
if pkg-config --exists sdl2 2>/dev/null; then
  SDL=(-DHOST_SDL=1 hosttest/host_sdl.c $(pkg-config --cflags --libs sdl2))
  echo "SDL2 $(pkg-config --modversion sdl2) found: c1943_host with live mode (--live)"
else
  echo "SDL2 not found: c1943_host without live mode (install libsdl2-dev for --live)"
fi

OPT=${OPT:--O1 -g -fsanitize=address -fno-omit-frame-pointer}
CFLAGS=(
  $OPT -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
  -D_DEFAULT_SOURCE
  -I c1943 -I third_party/miniz
  -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_DEFLATE_APIS
  -DMINIZ_NO_ZLIB_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
)

CORE=(
  c1943/z80.c
  c1943/c1943.c
  c1943/c1943_sets.c
  c1943/c1943_gfx.c
  c1943/c1943_video.c
  c1943/c1943_sound.c
  c1943/c1943_input.c
  c1943/ym2203.c
  c1943/ym_ssg.c
  c1943/c1943_store.c
  c1943/romload.c
  third_party/miniz/miniz.c
)

gcc "${CFLAGS[@]}" -I pico_shared/drivers/pico_hdmi hosttest/c1943_host.c "${CORE[@]}" "${SDL[@]}" -lm -o hosttest/c1943_host
gcc "${CFLAGS[@]}" -I hosttest hosttest/zex_host.c c1943/z80.c -o hosttest/zex_host
YMFM_DIR=${YMFM_DIR:-../ares/thirdparty/ymfm/src}
if [ -f "$YMFM_DIR/ymfm_opn.cpp" ]; then
  g++ -O2 -std=c++14 -I "$YMFM_DIR" hosttest/ym_compare.cpp "$YMFM_DIR/ymfm_opn.cpp" "$YMFM_DIR/ymfm_ssg.cpp" \
      "$YMFM_DIR/ymfm_misc.cpp" "$YMFM_DIR/ymfm_adpcm.cpp" -o hosttest/ym_compare
  echo "built hosttest/c1943_host, zex_host and ym_compare"
else
  echo "built hosttest/c1943_host and zex_host (no ymfm at $YMFM_DIR: no ym_compare)"
fi
