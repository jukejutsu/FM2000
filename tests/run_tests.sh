#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Everything that runs without an FM-1: the song model's tests, the firmware on the host (tests/host.c), and the
# simulator headless (needs node; web/build.sh first).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host build/sim
cc -O2 -Wall -Wextra -Wno-unused-function -Ifirmware/src -o build/host/test_song tests/test_song.c
build/host/test_song
cc -O2 -Wall -Wextra -Wno-unused-function -Ifirmware/src -o build/host/test_project tests/test_project.c
build/host/test_project
tests/run_host.sh
if command -v node >/dev/null 2>&1 && [ -f sim/fm2k.wasm ]; then node web/test_sim.mjs build/sim/sim; fi
