#!/bin/bash
#
# measure.sh — the xlrbridge test oracle (ported from HANDOFF.md §7.5).
#
# Records the BlackHole virtual device (or analyzes a given wav) and reports:
#   (a) dropouts   via ffmpeg silencedetect  (0 = clean routing)
#   (b) quality    via astats (peak / DC offset / dynamic range / flat factor)
#   (c) liveness   via volumedetect mean_volume (> -91 dBFS = stream flowing)
#
# IMPORTANT: ffmpeg is used ONLY for measurement here, never for the runtime
# routing (its async resampler drops audio — see HANDOFF §1.5).
#
# Dropout detection assumes a CONTINUOUS sound is playing while recording, so
# that silence == a real dropout, not a speech pause. Hum/whistle a sustained
# tone into the mic during the live capture.
#
# Usage:
#   tools/measure.sh                 # record :BlackHole 2ch live for DURATION s
#   tools/measure.sh sample.wav      # analyze an existing wav instead
#   DURATION=10 tools/measure.sh     # override the live record length (seconds)

set -euo pipefail

DEVICE="${DEVICE:-:BlackHole 2ch}"
DURATION="${DURATION:-8}"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg not found (brew install ffmpeg)" >&2
    exit 1
fi

WAV="${1:-}"
CLEANUP=0

if [ -z "$WAV" ]; then
    WAV="$(mktemp -t xlrbridge_measure).wav"
    CLEANUP=1
    echo "Recording '$DEVICE' for ${DURATION}s (hum a sustained tone)..."
    ffmpeg -hide_banner -loglevel error -f avfoundation -i "$DEVICE" \
        -t "$DURATION" -y "$WAV"
else
    if [ ! -f "$WAV" ]; then
        echo "error: file not found: $WAV" >&2
        exit 1
    fi
    echo "Analyzing file: $WAV"
fi

echo
echo "=== (a) Dropouts (silencedetect, 0 = clean) ==="
DROPS=$(ffmpeg -hide_banner -i "$WAV" \
    -af "silencedetect=noise=-50dB:d=0.04" -f null /dev/null 2>&1 \
    | grep -c silence_start || true)
echo "dropouts: $DROPS"

echo
echo "=== (b) Quality (astats) ==="
ffmpeg -hide_banner -i "$WAV" -af astats=metadata=1 -f null /dev/null 2>&1 \
    | grep -iE 'Peak level|DC offset|Dynamic range|Flat factor|Peak count' || true

echo
echo "=== (c) Liveness (mean_volume, > -91 dBFS = flowing) ==="
ffmpeg -hide_banner -i "$WAV" -af volumedetect -f null /dev/null 2>&1 \
    | grep mean_volume || true

if [ "$CLEANUP" = "1" ]; then
    rm -f "$WAV"
fi
