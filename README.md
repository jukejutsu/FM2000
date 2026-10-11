# FM2000

**A song maker for the M-VAVE FM-1, after Music 2000 on the PlayStation.** Write short riffs, place them on a
timeline of bars, build a whole track on the device: arrangement first, the way a DAW works, not a pattern
looper.

> 🚧 **Work in progress.** FM2000 is an early prototype. It runs in the browser simulator and on a computer; it has
> not been built or tried on an FM-1 yet. Things change daily, saved songs are not kept yet, and there is no release.
> Do not install it on your FM-1 for now.

## The idea

Most FM-1 firmwares are grooveboxes: patterns, loops, live play. FM2000 is for writing a song from start to end.

- **Four tracks:** three synth tracks, each with its own sound engine, and a drum track.
- **Riffs:** each track has a pool of up to 32 riffs, 1 to 64 steps long (a 1/16 step, up to 4 bars), with chords
  and note lengths: a small piano roll each, not a step pattern.
- **Arrangement:** the song is a timeline of up to 128 bars. Put a riff on a bar and it plays through its length,
  until the next riff on that track starts.
- **One screen, the whole song:** time runs left to right, the four tracks one under the other, every riff a
  coloured block with its name.

## What works now

- The song model: tracks, riffs, notes, the arrangement, packing a song for the flash (tested: `tests/test_song.c`)
- The ARR (arrangement) screen: cursor, scrolling, placing riffs, the playhead
- Playback from the cursor, a loop of 1 to 32 bars, a metronome
- A simple synth: one engine (BASIC, 7 presets) for tracks 1-3, three drum kits for track 4
- A test song to play with
- The simulator: the firmware's own C code compiled to WebAssembly, on a drawn FM-1 panel

## Planned

- RIF: the riff editor (notes on a piano roll, the drum grid), entered from a block on ARR
- Recording riffs from the keys, quantised
- Saving songs to the flash
- Real sound engines for tracks 1-3 (from the Felucca family) and better drum kits
- MIX, SND (sound editing), SNG (song settings) screens
- A build for the FM-1 and the first tests on the device

## Controls (ARR screen)

| Control | Does | In the simulator |
| --- | --- | --- |
| ALGORITHM | the track | ↑ ↓ |
| SELECT | the bar | ← → |
| PRESETS | the track's sound, or the drum kit on track 4 | Shift ← → |
| KNOB 1 | the riff placed on the bar (or none) | Shift ↑ ↓ |
| KNOB 2 | the loop: clockwise on (4 bars from the cursor's bar), anticlockwise off | P |
| KNOB 3 | the loop's length, 1-32 bars | A / G |
| KNOB 4 | free for now | |
| PLAY | play from the cursor / stop | Space |
| GLO | the metronome (for now) | 6 |
| keys | play the track's sound; OCT− OCT+ the octave | Z…/ and Q…O rows, [ ] |

## Try it

The simulator runs the same code the FM-1 will run.

```sh
sh web/serve.sh          # then open http://localhost:8000 and press "Switch on"
```

After changing the code, rebuild it with `sh web/build.sh` (needs clang with the wasm32 target and lld; on macOS
`brew install llvm lld`). `tests/run_tests.sh` runs the model's tests, the host build and the simulator headless.

## Inside

```
firmware/src/song.h       the song model: tracks, riffs, notes, the arrangement, pack / unpack
firmware/src/fm2000.c     the app: the ARR screen, the sequencer, the synth
firmware/src/platform.c   the FM-1 platform (boot, USB, updates, audio, panel, screen, settings), from Dinghy
web/                      the simulator's sources: the browser platform, the page, the audio worklet
sim/                      the simulator, built
tests/                    the model's tests and the host run
tools/                    the build, the installer, the font generator
```

The song model in short: `cell[bar][track]` starts a riff (1..32) or is empty; a riff plays on through its length
unless the next one starts first. Every note of every riff lives in one shared pool of 4096 notes. A song packs to a
few KB with a CRC.

The chip has no FPU: everything is integer arithmetic (see `docs/RULES.md`, and `docs/API.md` for the platform).

## Credits

- [Dinghy](https://github.com/zvenson/dinghy) by zvenson: the starting point, the minimal FM-1 firmware this is built on
- [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (Hügelton Instruments): the FM-1 platform, and the
  browser simulator's design; [SLOOP](https://github.com/isod89/sloop-fm1) by isod89: the platform as it carries it
- [X0X](https://github.com/charlesvestal/fm1-x0x) by charlesvestal: the first browser build of the family
- Inter Tight (The Inter Project Authors, SIL OFL 1.1): the UI font
- Music 2000 (Jester Interactive / Codemasters, 1999) for the idea. FM2000 uses none of its code, graphics or sounds.

## License

GPL-3.0-only, as Felucca, SLOOP and Dinghy. Details and third-party parts in [LICENSING.md](LICENSING.md).

M-VAVE and FM-1 are trademarks of their respective owners. FM2000 is not affiliated with or endorsed by them.
