#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The simulator: firmware/src/fm2k.c compiled to WebAssembly with the browser's platform (web/platform_web.c),
# and the page, into sim/ (open it with web/serve.sh, or publish sim/ with GitHub Pages).
# Needs clang with the wasm32 target and wasm-ld: on macOS `brew install llvm lld` (Apple's clang has neither);
# on Linux the distribution's clang and lld. CLANG points elsewhere.
set -e
cd "$(dirname "$0")/.."
if [ -z "$CLANG" ]; then
    for c in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang "$(command -v clang)"; do
        [ -x "$c" ] && { CLANG="$c"; break; }
    done
fi
[ -n "$CLANG" ] || { echo "web: no clang"; exit 1; }
PATH="$(dirname "$CLANG"):/opt/homebrew/opt/lld/bin:/usr/local/opt/lld/bin:$PATH"
mkdir -p sim
"$CLANG" --target=wasm32 -O2 -ffreestanding -nostdlib -fno-builtin-memset -Wall -Wno-unused-function \
    -DFM2K_WEB -DAPP_VER="\"${APP_VER:-web}\"" -Ifirmware/src -Ifirmware/gen \
    -Wl,--no-entry -Wl,-z,stack-size=262144 -Wl,--initial-memory=4194304 \
    -o sim/fm2k.wasm firmware/src/fm2k.c
cp web/index.html web/worklet.js sim/
echo "web: sim/ ($(wc -c < sim/fm2k.wasm | tr -d ' ') B wasm)"
