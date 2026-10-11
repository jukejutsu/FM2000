#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The simulator on http://localhost:8000 (an AudioWorklet needs http, not a file:// page). Ctrl-C stops it.
cd "$(dirname "$0")/../sim" && echo "FM2000 simulator: http://localhost:${1:-8000}" && exec python3 -m http.server "${1:-8000}"
