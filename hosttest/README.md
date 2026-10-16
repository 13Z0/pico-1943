# Host test harness

The emulated board in `c1943/` is plain C without Pico dependencies, and also builds for Linux. The commands below are run from the root of the repository:

```sh
hosttest/build.sh
./hosttest/c1943_host ~/roms/arcade 1943 3600 300 hosttest/out --coin 1200 --start 1260 --play 1320 --wav hosttest/out/game.wav
python3 hosttest/ppm2png.py hosttest/out/frame_02400.ppm
./hosttest/zex_host zexdoc.com zexall.com
```

`c1943_host` reads the sets from a folder laid out as the SD card (`1943`, `1943KAI`) through the same loader as the firmware, runs the given number of frames with scripted input, writes every Nth frame as a 320 x 240 PPM in the chosen orientation (`--tate 0|1|2`) and the sound as a WAV file. `--tall` writes instead the 640 x 480 picture that a board with HSTX video output shows with *Tate mode* off. `--no-psram --flash FILE` runs the game from a simulated flash copy, as a board without PSRAM does. Every second it reports the time per frame of the main CPU, the sound board and the drawing, and what the machine did (the protection's questions and answers, bank switches, the cycles the CPUs spent waiting).

`zex_host` runs the ZEXDOC and ZEXALL instruction exercisers on the Z80 core; the test programs are not part of this repository.

## Live mode

With SDL2 installed (`libsdl2-dev`), `--live` plays the game in a window with sound, in real time, in any orientation:

```sh
./hosttest/c1943_host ~/roms/arcade 1943 --live
./hosttest/c1943_host ~/roms/arcade 1943kai --live --tate 1
./hosttest/c1943_host ~/roms/arcade 1943 --live --tall
```

The keys are those of a USB keyboard on the board, mapped as on the board: the arrow keys move, Z (or C) is button 1, X (or V) button 2, A inserts a coin when released and S is the start button; 5, 1 and 2 work as in MAME (coin, 1 player start, 2 player start). A + S resets the game. Space pauses, N steps one frame while paused, Tab (held) runs fast, F12 saves the frame on screen and Esc quits.

## Sound compared with ymfm

The sound can be checked against ymfm, the YM2203 emulation MAME uses today. `hosttest/build.sh` builds `ym_compare` when it finds ymfm's sources (`YMFM_DIR`, by default the copy in ares next to this repository):

```sh
./hosttest/c1943_host ~/roms/arcade 1943 2400 0 hosttest/out --coin 400 --start 460 --play 520 --wav a.wav --ymlog a.log
./hosttest/ym_compare a.log b.wav
python3 hosttest/wavcompare.py a.wav b.wav
```

`--ymlog` records every write to the two YM2203s, `ym_compare` plays them through ymfm with MAME's mix, and `wavcompare.py` compares the two recordings: their level, the strongest frequency and the spectrum over windows of 50 ms. `--only fm` and `--only ssg` (and `fm` or `ssg` as the third argument of `ym_compare`) compare one half of the chips at a time.
