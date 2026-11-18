#!/bin/bash
# build.sh with extra options for the generated code: scripts/build-exp.sh "-Os -DFOO"
GEN_EXTRA="$1" exec bash "$(dirname "$0")/build.sh"
