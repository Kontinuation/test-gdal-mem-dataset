#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PROFILES_DIR="${PROFILE_DIR:-$ROOT_DIR/profiles}"
FLAMEGRAPH_DIR="$HOME/workspace/github/FlameGraph"
BENCH="$ROOT_DIR/gdal_mem_test"

mkdir -p "$PROFILES_DIR"

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <duration_seconds> <benchmark args...>"
  exit 1
fi

DUR="$1"
shift

MODE="benchmark"
for ((i = 1; i <= $#; i++)); do
  if [[ "${!i}" == "--mode" ]]; then
    next=$((i + 1))
    MODE="${!next}"
    break
  fi
done

"$BENCH" "$@" &
PID=$!

sleep 1

SAMPLE_OUT="$PROFILES_DIR/${MODE}.sample.txt"
FOLDED_OUT="$PROFILES_DIR/${MODE}.folded"
SVG_OUT="$PROFILES_DIR/${MODE}.svg"

/usr/bin/sample "$PID" "$DUR" -file "$SAMPLE_OUT" || true

if [[ -d "$FLAMEGRAPH_DIR" ]]; then
  awk -f "$FLAMEGRAPH_DIR/stackcollapse-sample.awk" "$SAMPLE_OUT" > "$FOLDED_OUT" || true
  "$FLAMEGRAPH_DIR/flamegraph.pl" "$FOLDED_OUT" > "$SVG_OUT" || true
  echo "Generated: $SVG_OUT"
else
  echo "Generated: $SAMPLE_OUT"
fi

wait "$PID" || true
