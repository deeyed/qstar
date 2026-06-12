#!/bin/sh
set -eu

mkdir -p "$(dirname "$1")"
printf "aggregate\n" > "$1"
