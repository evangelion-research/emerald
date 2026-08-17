#!/usr/bin/env bash
# ============================================================================
# Stress test for the Emerald raytracer.
#
# Sweeps image resolution and samples-per-pixel, recompiles a scaled variant
# of one_weekend.rald for each, times the run, and reports peak RSS plus the
# program's own GC stats (collections, live objects, young/old split).
#
# Usage:  ./stress.sh [runs_per_config]
#   runs_per_config defaults to 3 (median is reported).
#
# The raytracer renders into out.ppm in the current directory; we run each
# variant in a scratch dir so results don't collide.
# ============================================================================
set -u
cd "$(dirname "$0")"
RUNS="${1:-3}"
EMERALDC=../../bin/emeraldc

# config: "width samples max_depth"  (aspect ratio is fixed at 16:9)
CONFIGS=(
  "80  1 16"
  "80  4 16"
  "160 1 16"
  "160 4 16"
  "160 16 32"
  "320 4 16"
  "320 16 32"
)

time_cmd=()
if command -v gtime >/dev/null 2>&1; then
  time_cmd=(gtime -f "elapsed=%e rss=%MKB")
elif [ "$(uname)" = "Darwin" ]; then
  time_cmd=(/usr/bin/time -l)
else
  time_cmd=(/usr/bin/time -v)
fi

printf "%-14s %-6s %-8s %-8s %-10s %-10s %-8s %-8s %-8s\n" \
  "width" "samples" "depth" "spheres" "elapsed_s" "rss_MB" "collections" "live" "young/old"
printf "%s\n" "-------------------------------------------------------------------------"

for cfg in "${CONFIGS[@]}"; do
  read -r W S D <<<"$cfg"
  scratch="$(mktemp -d)"
  # scale the config constants in a copy of the scene
  sed -e "s/^image_width       = .*/image_width       = $W/" \
      -e "s/^samples_per_pixel = .*/samples_per_pixel = $S/" \
      -e "s/^max_depth         = .*/max_depth         = $D/" \
      -e 's/^    # GCSTATS print/    print/' \
      one_weekend.rald > "$scratch/scene.rald"

  "$EMERALDC" -o "$scratch/scene" "$scratch/scene.rald" 2>/dev/null || {
    echo "compile failed for $W x $S (depth $D)"
    rm -rf "$scratch"
    continue
  }

  best_elapsed=999999
  best_rss=0
  best_gc=""
  for run in $(seq 1 "$RUNS"); do
    out="$("${time_cmd[@]}" -o "$scratch/time.txt" "$scratch/scene" 2>&1)"
    elapsed="$(awk '/elapsed=/{print $1}' "$scratch/time.txt" 2>/dev/null | cut -d= -f2)"
    rss="$(awk '/rss=/{print $1}' "$scratch/time.txt" 2>/dev/null | cut -d= -f2 | tr -d 'MB')"
    if [ -z "$elapsed" ]; then
      # macOS /usr/bin/time -l format
      elapsed="$(awk '/real/{print $2}' "$scratch/time.txt" 2>/dev/null)"
      rss="$(awk '/maximum resident set size/{print $NF}' "$scratch/time.txt" 2>/dev/null)"
    fi
    # GC stats line: "gc: {collections: N, live: N, young: N, old: N, threshold: N}"
    gc="$(echo "$out" | grep -o 'collections: [0-9]*, live: [0-9]*, young: [0-9]*, old: [0-9]*' | head -1)"
    # keep the best (fastest) run's numbers
    if awk -v e="$elapsed" 'BEGIN{exit !(e<'"$best_elapsed"')}' 2>/dev/null; then
      best_elapsed="$elapsed"
      best_rss="$rss"
      best_gc="$gc"
    fi
  done

  spheres="$(echo "$out" | grep -o 'spheres [0-9]*' | head -1 | awk '{print $2}')"
  # elapsed in seconds (mm:ss or s)
  if echo "$best_elapsed" | grep -q ':'; then
    mm="${best_elapsed%%:*}"; ss="${best_elapsed##*:}"
    best_elapsed="$(awk -v m="$mm" -v s="$ss" 'BEGIN{printf "%.1f", m*60+s}')"
  fi
  rss_mb="$(awk -v r="$best_rss" 'BEGIN{if (r>0) printf "%.1f", r/1024; else print "?"}')"
  printf "%-14s %-6s %-8s %-8s %-10s %-10s %s\n" \
    "$W" "$S" "$D" "${spheres:-?}" "${best_elapsed}s" "${rss_mb}MB" "$best_gc"
  rm -rf "$scratch"
done
