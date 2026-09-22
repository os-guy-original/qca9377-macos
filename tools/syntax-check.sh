#!/bin/sh
# syntax-check.sh — Linux-side pre-CI gate: compiles every skeleton TU
# against stub IOKit headers (no Xcode needed). Catches type/typo errors
# before pushing to the macOS CI runner. Not a substitute for CI (no
# codesign, no real SDK), but catches ~90% of build breaks in seconds.
set -e
DIR="$(cd "$(dirname "$0")/.." && pwd)/skeleton"
INC="$(cd "$(dirname "$0")" && pwd)/stub-include"
PASS=1
for F in WMI.cpp CE.cpp HTC.cpp BMI.cpp FW.cpp QCA9377Driver.cpp; do
  OUT=$(g++ -fsyntax-only -std=c++17 -Wno-unused-parameter -I"$INC" -I"$DIR" "$DIR/$F" 2>&1 || true)
  if [ -n "$OUT" ]; then echo "$F: ISSUES"; echo "$OUT" | head -20; PASS=0; fi
done
[ "$PASS" = "1" ] && { echo "syntax-check: all TUs clean"; exit 0; }
exit 1
