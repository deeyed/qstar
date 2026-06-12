#!/bin/sh
set -eu

in=
out=
while [ "$#" -gt 0 ]; do
  case "$1" in
    -O)
      shift
      ;;
    *)
      if [ -z "$in" ]; then
        in=$1
      else
        out=$1
      fi
      ;;
  esac
  shift || break
done

mkdir -p "$(dirname "$out")"
cat "$in" > "$out"
