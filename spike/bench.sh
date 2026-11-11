#!/usr/bin/env bash
# Runs lvpbench over image sizes and sample counts; appends JSON lines to $3 (spike M1-03, #38).
# Usage: bench.sh <lvpbench> <label> <out.jsonl> [quick]
set -uo pipefail
bin=$1 label=$2 out=$3 quick=${4:-}
mkdir -p images
run() {
  local name=$1; shift
  local line status
  if line=$("$bin" "$@" --out "images/${label}-${name}.ppm"); then status=ok; else status=fail; fi
  [ -z "$line" ] && line="{\"name\":\"${name}\"}"
  echo "$line" | sed "s/^{/{\"label\":\"${label}\",\"status\":\"${status}\",/" | tee -a "$out"
}
# Cold: no Mesa shader cache, as on a fresh CI runner.
export MESA_SHADER_CACHE_DISABLE=true
run cold-raster --mode raster --size 256 --spp 1 --validation
run cold-rq --mode rq --size 256 --spp 1 --validation
sizes="128 256 512"; spps="1 16 64 256"
if [ -n "$quick" ]; then sizes="128 256"; spps="1 16 64"; fi
for size in $sizes; do
  for spp in $spps; do
    run "raster-${size}-${spp}" --mode raster --size "$size" --spp "$spp" --validation
  done
done
for size in $sizes; do
  for spp in $spps; do
    run "rq-${size}-${spp}" --mode rq --size "$size" --spp "$spp" --validation
  done
done
# Validation overhead and synchronisation validation.
run raster-256-64-novalidation --mode raster --size 256 --spp 64
run rq-256-64-novalidation --mode rq --size 256 --spp 64
run raster-256-64-sync --mode raster --size 256 --spp 64 --validation --sync
run rq-256-64-sync --mode rq --size 256 --spp 64 --validation --sync
exit 0
