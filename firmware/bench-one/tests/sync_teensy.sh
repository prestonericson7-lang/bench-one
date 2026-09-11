#!/bin/sh
# Copy the sources the Teensy sketch needs into the sketch folder.
#
# The Arduino IDE compiles exactly one folder and offers no way to add an include path, and the
# Teensy platform's compile recipe has no extra_flags hook to bolt one on. So the files are
# copied. Run this after editing anything in shared/ or system_test.c, or the sketch quietly
# builds the old code -- which is the entire reason this script exists rather than a note in a
# README telling you to remember.
set -e
D="$(dirname "$0")/system_test_teensy"
for f in bench_hdc bench_hdc_shard bench_index; do
    cp "$(dirname "$0")/../shared/$f.h" "$D/"
    cp "$(dirname "$0")/../shared/$f.c" "$D/"
done
cp "$(dirname "$0")/system_test.c" "$D/"
echo "synced into $D"

# The PSRAM sketch needs only the HDC kernel, but it must be the REAL one -- a hand-copied
# hamming loop would measure a different function than the system actually runs.
P="$(dirname "$0")/psram_teensy"
cp "$(dirname "$0")/../shared/bench_hdc.h" "$P/"
cp "$(dirname "$0")/../shared/bench_hdc.c" "$P/"
echo "synced into $P"

S2="$(dirname "$0")/sd_teensy"
cp "$(dirname "$0")/../shared/bench_hdc.h" "$S2/"
cp "$(dirname "$0")/../shared/bench_hdc.c" "$S2/"
echo "synced into $S2"

E="$(dirname "$0")/kernel_esp32s3"
cp "$(dirname "$0")/../shared/bench_hdc.h" "$E/"
cp "$(dirname "$0")/../shared/bench_hdc.c" "$E/"
echo "synced into $E"
