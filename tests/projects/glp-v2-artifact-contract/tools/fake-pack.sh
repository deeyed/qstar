#!/bin/sh
set -eu

mode=$1
shift

case "$mode" in
  compile)
    source=$1
    output=$2
    mkdir -p "$(dirname "$output")"
    "${CC:-cc}" -x c -c "$source" -o "$output"
    ;;
  final)
    runtime=$1
    metadata=$2
    resources=$3
    link=$4
    shift 4
    mkdir -p "$(dirname "$runtime")" "$resources"
    printf 'runtime\n' > "$runtime"
    printf 'metadata\n' > "$metadata"
    printf 'resource\n' > "$resources/index.txt"
    printf 'link-interface\n' > "$link"
    for value in "$@"; do
      printf '%s\n' "$value" >> "$metadata"
    done
    ;;
  *)
    exit 2
    ;;
esac
