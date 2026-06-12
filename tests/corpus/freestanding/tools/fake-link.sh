#!/bin/sh
set -eu

out=
while [ "$#" -gt 0 ]; do
  if [ "$1" = "-o" ]; then
    shift
    out=$1
  fi
  shift || break
done

mkdir -p "$(dirname "$out")"
printf "fake linked image\n" > "$out"
