#!/bin/sh
# check.sh: compile ShannonIms on the host against stand-ins for the Android classes it uses
# (signatures from LineageOS 24.0 frameworks/base) and for the Java backend of
# vendor.shannond.ims (from the .aidl files). Catches wrong names, types and signatures
# before an Android build; nothing runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$HERE/..
AIDL=$APP/../../frontends/aidl/ims/aidl
OUT=${1:-$(mktemp -d)}
mkdir -p "$OUT/gen" "$OUT/classes"
python3 "$HERE/gen_aidl_java.py" "$AIDL" "$OUT/gen"
javac -Xlint:-options -d "$OUT/classes" $(find "$HERE/stubs" "$OUT/gen" "$APP/src" -name '*.java')
echo "ShannonIms host check: ok"
