#!/usr/bin/env bash
# Replays TomoBank fatigue scans through vol-stream with two subscribers:
# crack_monitor.py --mode crack (a band of rows) and --mode preview (one
# projection). See README.md for where the scans come from.
#
# Usage: run_fatigue_demo.sh BUILD_DIR DATA_DIR [OUT_DIR] [SCAN ...]
#   DATA_DIR holds tomo_000NN.h5 and cycles.csv; SCANs default to every
#   tomo_*.h5 there, in name order. OUT_DIR (default DATA_DIR/run) receives
#   the stream file, logs, PNGs and damage.csv.
# Environment: PYTHON (default python3), ROWS (crack band, default 400:960),
#   CRACK_DEFLATE / PREVIEW_DEFLATE (deflate that subscriber's images in transit).
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ $# -ge 2 ] || { sed -n '2,11p' "$0"; exit 2; }
BUILD_DIR="$(cd "$1" && pwd)" || exit 1
DATA_DIR="$(cd "$2" && pwd)" || exit 1
OUT_DIR="${3:-$DATA_DIR/run}"
shift $(( $# < 3 ? $# : 3 ))
mkdir -p "$OUT_DIR" && OUT_DIR="$(cd "$OUT_DIR" && pwd)"

if [ $# -gt 0 ]; then SCANS=("$@"); else SCANS=("$DATA_DIR"/tomo_*.h5); fi
[ -e "${SCANS[0]}" ] || { echo "run_fatigue_demo.sh: no tomo_*.h5 in $DATA_DIR" >&2; exit 1; }
[ -r "$DATA_DIR/cycles.csv" ] || { echo "run_fatigue_demo.sh: no cycles.csv in $DATA_DIR" >&2; exit 1; }

WRITER="$BUILD_DIR/examples/fatigue_crack/tomo_fatigue_writer"
[ -x "$WRITER" ] || { echo "run_fatigue_demo.sh: build tomo_fatigue_writer first ($WRITER)" >&2; exit 1; }
export PYTHONPATH="$BUILD_DIR/python${PYTHONPATH:+:$PYTHONPATH}"
export HDF5_PLUGIN_PATH="${HDF5_PLUGIN_PATH:-$BUILD_DIR}"
PYTHON="${PYTHON:-python3}"

# na+sm's bulk path needs cross-memory attach, which Yama's ptrace_scope >= 1
# forbids; fall back to TCP rather than fail every large push.
if [ -z "${VOL_STREAM_NA:-}" ]; then
    if [ "$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)" = "0" ]; then
        export VOL_STREAM_NA=na+sm
    else
        export VOL_STREAM_NA=ofi+tcp
    fi
fi
# A 15.7 GiB step: the per-step deflate()d .payload copy would dominate the run.
export VOL_STREAM_STAGE_PAYLOAD="${VOL_STREAM_STAGE_PAYLOAD:-0}"

cd "$OUT_DIR" || exit 1
STREAM=fatigue_stream.h5
echo "run_fatigue_demo.sh: ${#SCANS[@]} checkpoint(s), transport $VOL_STREAM_NA, output in $OUT_DIR"

"$WRITER" "$STREAM" 2 "$DATA_DIR/cycles.csv" "${SCANS[@]}" > writer.log 2>&1 &
WPID=$!
trap 'kill $WPID $CPID $PPID_ 2>/dev/null; wait 2>/dev/null' INT TERM
for _ in $(seq 300); do [ -e "$STREAM.vsgroup" ] && break; sleep 0.1; done

"$PYTHON" "$SCRIPT_DIR/crack_monitor.py" "$STREAM" --mode crack --rows "${ROWS:-400:960}" \
    --out "$OUT_DIR" --expect-steps "${#SCANS[@]}" ${CRACK_DEFLATE:+--deflate "$CRACK_DEFLATE"} > crack.log 2>&1 &
CPID=$!
"$PYTHON" "$SCRIPT_DIR/crack_monitor.py" "$STREAM" --mode preview \
    --out "$OUT_DIR" --expect-steps "${#SCANS[@]}" ${PREVIEW_DEFLATE:+--deflate "$PREVIEW_DEFLATE"} > preview.log 2>&1 &
PPID_=$!

# The subscribers close first, while the writer's group is still alive.
wait $CPID; crc=$?
wait $PPID_; prc=$?
wait $WPID; wrc=$?
cat writer.log crack.log preview.log
echo "run_fatigue_demo.sh: writer $wrc, crack monitor $crc, preview $prc"
[ $wrc -eq 0 ] && [ $crc -eq 0 ] && [ $prc -eq 0 ]
