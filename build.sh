#!/usr/bin/env bash
#
# Build ats-mini-cn against the bundled SI4732 driver fork.
#
# The fork (ats-mini/libraries/SI4732, a hardened 2.1.8) and the official
# PU2CLR SI4735 library both ship a header named SI4735.h. The fork adds
# busTimeout and the bounded retries SI4735-fixed.h relies on, so it has to be
# the one that wins: --library gives that path top priority when arduino-cli
# resolves includes, and the profile file (sketch.yaml) must stay disabled
# because a profile pins the official library and ignores sketch/libraries.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ARDUINO_CLI="${ARDUINO_CLI:-$HOME/bin/arduino-cli}"

# Same board options as the upstream profile, CPU stays at 80 MHz
# (-DF_CPU=80000000L, then main.cpp calls setCpuFrequencyMhz(80)).
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none"

SKETCH="$ROOT/ats-mini"
LIB="$SKETCH/libraries/SI4732"

if [ -f "$SKETCH/sketch.yaml" ]; then
  echo "ats-mini/sketch.yaml is present: arduino-cli would switch to profile mode," >&2
  echo "install the official PU2CLR SI4735 and ignore the bundled fork." >&2
  echo "Rename it to sketch.yaml.disabled first." >&2
  exit 1
fi

# /tmp is often a small tmpfs; arduino-cli unpacks a lot while it builds.
export TMPDIR="${TMPDIR:-$HOME/build-tmp}"
mkdir -p "$TMPDIR"

"$ARDUINO_CLI" compile \
  --fqbn "$FQBN" \
  --library "$LIB" \
  --warnings all \
  --build-path "$ROOT/build" \
  --output-dir "$ROOT/out" \
  "$SKETCH"

echo
echo "Artifacts in $ROOT/out"
md5sum "$ROOT"/out/*.bin
