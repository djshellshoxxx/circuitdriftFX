#!/usr/bin/env bash
# Collects the built plugin artefacts of both products into a staging directory and runs structural
# smoke checks. Used by CI and the release workflow (bash, runs under Git Bash on the Windows runner).
#
# Usage: stage_windows.sh <build-dir> <dest-dir> [config=Release]
# Layout produced: <dest>/VST3/*.vst3 (bundles), <dest>/CLAP/*.clap, <dest>/Standalone/*.exe
set -euo pipefail

build=${1:?build dir}
dest=${2:?destination dir}
config=${3:-Release}

rm -rf "$dest"
mkdir -p "$dest/VST3" "$dest/CLAP" "$dest/Standalone"

copy_found() {  # copy_found <kind-dir> <find args...>
  local target=$1
  shift
  local n=0 item
  while IFS= read -r -d '' item; do
    cp -R "$item" "$dest/$target/"
    n=$((n + 1))
  done < <(find "$build" "$@" -print0)
  echo "$target: $n item(s)"
  if [ "$n" -ne 2 ]; then
    echo "error: expected 2 $target artefacts (CircuitDrift and CircuitDrift LITE), found $n" >&2
    find "$build" -name "*_artefacts" -maxdepth 3 >&2 || true
    exit 1
  fi
}

copy_found VST3 -path "*_artefacts/$config/VST3/*.vst3" -type d -prune
copy_found CLAP -type f -name '*.clap'
copy_found Standalone -type f -path "*_artefacts/$config/Standalone/*.exe"

# Structural smoke checks (not a host load test): exported entry points are present in the binaries.
status=0
for vst3 in "$dest"/VST3/*.vst3; do
  dll=$(find "$vst3/Contents" -type f -name '*.vst3' | head -n 1)
  if [ -z "$dll" ] || ! grep -qa 'GetPluginFactory' "$dll"; then
    echo "error: $vst3 has no GetPluginFactory export" >&2
    status=1
  fi
  if [ ! -f "$vst3/Contents/moduleinfo.json" ]; then
    echo "warning: $vst3 has no Contents/moduleinfo.json (VST3 manifest)" >&2
  fi
done
for clap in "$dest"/CLAP/*.clap; do
  if ! grep -qa 'clap_entry' "$clap"; then
    echo "error: $clap has no clap_entry export" >&2
    status=1
  fi
done
exit $status
