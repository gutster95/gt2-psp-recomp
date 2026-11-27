#!/bin/sh
# GT2 PSP converter - Linux. Starts the local web app and opens it in your browser.
cd "$(dirname "$0")" || exit 1
mkdir -p input
PY=$(command -v python3 || command -v python)
if [ -z "$PY" ]; then echo "Python 3 is needed: install it with your package manager (e.g. sudo apt install python3)."; exit 1; fi
exec "$PY" converter/server.py
