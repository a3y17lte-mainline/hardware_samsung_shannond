#!/bin/sh
# check.sh SHANNOND_DIR LIBSAMSUNG_IPC_DIR AIDL_API_DIR OUT: compile the AIDL front end on the host
# against generated stand-ins for the NDK headers (-fsyntax-only: nothing is linked or run)
set -e
SRC=$1 IPC=$2 API=$3 OUT=$4
HERE=$(dirname "$0")
rm -rf "$OUT/mocks"
python3 "$HERE/gen_mocks.py" "$API" 5 "$OUT/mocks" "$SRC/frontends/aidl/ims/aidl"
fail=0
for f in "$SRC"/frontends/aidl/*.cpp; do
	clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
		-I"$HERE/include" -I"$OUT/mocks" -I"$OUT/mocks/aidl/.." -I"$SRC" -I"$IPC/include" \
		"$f" || fail=1
done
exit $fail
