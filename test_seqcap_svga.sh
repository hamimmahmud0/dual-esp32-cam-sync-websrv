#!/usr/bin/env bash
set -euo pipefail

# Change this if your master hostname/IP is different
MASTER_HOST="${MASTER_HOST:-cam-master-331677b2.local}"

# Capture params
PIXFORMAT="${PIXFORMAT:-jpeg}"   # jpeg | rgb565 | yuv422 | grayscale | rgb888 | raw
SIZE="${SIZE:-SVGA}"               # e.g. SVGA
SEQNAME="${SEQNAME:-test_svga}"    # folder under /sdcard/captures/
AMOUNT="${AMOUNT:-5}"              # number of frames

# Optional camera controls
CONTRAST="${CONTRAST:-1}"
SATURATION="${SATURATION:--2}"

# Timing
SLAVE_PREP_MS="${SLAVE_PREP_MS:-250}"
INTER_FRAME_MS="${INTER_FRAME_MS:-50}"

URL="http://${MASTER_HOST}/seq_cap?pixformat=${PIXFORMAT}&size=${SIZE}&cap_seq_name=${SEQNAME}&cap_amount=${AMOUNT}&contrast=${CONTRAST}&saturation=${SATURATION}&slave_prepare_delay_ms=${SLAVE_PREP_MS}&inter_frame_delay_ms=${INTER_FRAME_MS}"

echo "Requesting: $URL"
curl -v "$URL"
echo
echo "Done. Check SD card: /sdcard/captures/${SEQNAME}/"
