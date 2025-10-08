#!/usr/bin/env bash
# Usage: ./scripts/run_com_pair.sh <file-to-send>
# Starts receiver (host PTY), waits for slave path, sets raw mode, then starts sender to transmit file.
set -euo pipefail
if [ $# -lt 1 ]; then
  echo "Usage: $0 <file-to-send>"
  exit 2
fi
FILE_TO_SEND="$1"
BUILD_DIR="/mnt/d/programming/Windows/VAL_protocol/build/linux-debug"
RECV_BIN="$BUILD_DIR/bin/val_example_receive_com"
SEND_BIN="$BUILD_DIR/bin/val_example_send_com"
RECV_LOG="$BUILD_DIR/recv.log"
SEND_LOG="$BUILD_DIR/send.log"
PID_FILE="$BUILD_DIR/recv.pid"
# Quiet by default: hex/raw serial dump is disabled unless --serial-verbose is passed now
# export VAL_COM_DEBUG_RAW=1
# start receiver in background, capture both stdout+stderr
# ensure log file exists so grep/tail won't fail immediately
 : > "$RECV_LOG"
# Ensure output directory exists for receiver
OUTDIR="/tmp/val_com_out"
mkdir -p "$OUTDIR"
( stdbuf -oL -eL "$RECV_BIN" --host "$OUTDIR" 2>&1 | tee -a "$RECV_LOG" ) &
RECV_PID=$!
echo $RECV_PID > "$PID_FILE"
# wait for slave path
SLAVE=""
for i in {1..80}; do
  # extract only the /dev/pts/N path (avoid grabbing the full printed line)
  SLAVE=$(grep -m1 -o -E '/dev/pts/[0-9]+' "$RECV_LOG" || true)
  if [ -n "$SLAVE" ]; then
    # ensure the device file exists
    if [ -e "$SLAVE" ]; then break; fi
  fi
  sleep 0.1
done
if [ -z "$SLAVE" ]; then
  echo "Failed to detect PTY slave in $RECV_LOG"
  kill $RECV_PID || true
  exit 3
fi
echo "Detected PTY: $SLAVE"
# ensure slave in raw mode (wait briefly for device to be ready)
sleep 0.05
if [ -e "$SLAVE" ]; then
  stty -F "$SLAVE" raw -echo || true
else
  echo "Warning: PTY $SLAVE not present when applying stty"
fi
# start sender and wait
stdbuf -oL -eL "$SEND_BIN" "$SLAVE" 115200 "$FILE_TO_SEND" 2>&1 | tee "$SEND_LOG"
# cleanup
if ps -p $RECV_PID >/dev/null 2>&1; then kill $RECV_PID || true; fi
rm -f "$PID_FILE"
echo "Logs: $RECV_LOG and $SEND_LOG"
exit 0
