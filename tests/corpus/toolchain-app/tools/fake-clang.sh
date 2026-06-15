#!/bin/sh
set -eu

out=
dep=
src=
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o)
      shift
      out=$1
      ;;
    -MF)
      shift
      dep=$1
      ;;
    -c)
      shift
      src=$1
      ;;
  esac
  shift || break
done

mkdir -p "$(dirname "$out")"
printf "fake object\n" > "$out"
if [ -n "$dep" ]; then
  mkdir -p "$(dirname "$dep")"
  printf "%s: %s\n" "$out" "$src" > "$dep"
fi
