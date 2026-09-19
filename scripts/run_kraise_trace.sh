#!/bin/sh
#
# run_kraise_trace.sh
# Run on the QNX Raspberry Pi 4 target AFTER kraise_system is already running.
#
# Usage:
#   sh /data/home/qnxuser/kraise/run_kraise_trace.sh
#
# Output:
#   /data/home/qnxuser/kraise/kraise_trace.kev
#
# Then copy to Windows and open in Momentics System Profiler.
#
set -e

DEST=/data/home/qnxuser/kraise
TRACE_FILE="$DEST/kraise_trace.kev"

echo "[TRACE] Verifying KRAISE is running..."
pidin ar | grep kraise_system || {
    echo "[TRACE] ERROR: kraise_system not found in process list."
    echo "        Start it first: $DEST/kraise_system &"
    exit 1
}

echo "[TRACE] Starting kernel trace (20 seconds)..."
echo "[TRACE] Output: $TRACE_FILE"

# -f  : output file
# -s  : stop after N seconds
# -n 1: flush every 1 second to avoid data loss
tracelogger -f "$TRACE_FILE" -s 20 -n 1 &
TRACER_PID=$!

echo "[TRACE] Tracer PID: $TRACER_PID"
echo "[TRACE] Waiting 8 seconds then injecting fault on svc_pir_a..."
sleep 8

echo "[TRACE] Injecting fault: touch /tmp/kill_svc_pir_a"
touch /tmp/kill_svc_pir_a

# Force-kill svc_pir_a so HAM can demonstrate recovery
SVC_PID=$(pidin ar | awk '/svc_pir_a/{print $1}' | head -1)
if [ -n "$SVC_PID" ]; then
    echo "[TRACE] Killing svc_pir_a PID $SVC_PID..."
    slay -f svc_pir_a 2>/dev/null || kill -9 "$SVC_PID" 2>/dev/null || true
fi

echo "[TRACE] Letting recovery run for 10 more seconds..."
sleep 10

echo "[TRACE] Stopping tracer..."
wait $TRACER_PID 2>/dev/null || true

if [ -f "$TRACE_FILE" ]; then
    ls -lh "$TRACE_FILE"
    echo "[TRACE] SUCCESS. Copy to Windows:"
    echo "   From Windows terminal:"
    echo "   scp -c aes256-ctr -o MACs=hmac-sha2-256 qnxuser@169.254.43.14:$TRACE_FILE ."
    echo "   Then open in QNX Momentics → System Profiler."
else
    echo "[TRACE] ERROR: Trace file not created."
    exit 1
fi
