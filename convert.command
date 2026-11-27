#!/bin/sh
# GT2 PSP converter - macOS (double-click in Finder). Starts the local web app and opens it in your browser.
cd "$(dirname "$0")" || exit 1
mkdir -p input
PY=$(command -v python3)
if [ -z "$PY" ]; then
    echo "Python 3 is needed. Install it from https://www.python.org/downloads/macos/ and start this again."
    read -r _; exit 1
fi
exec "$PY" converter/server.py
