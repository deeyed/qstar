#!/bin/sh
set -eu

input=$1
output=$2
mode=$3
verbose=${4:-}
project_root=..
input_path=$project_root/$input
output_path=$project_root/$output

mkdir -p "${output_path%/*}"
printf 'input=%s\nmode=%s\nverbose=%s\ncommand_env=%s\nstep_env=%s\n' \
  "$(cat "$input_path")" "$mode" "$verbose" \
  "${QSTAR_COMMAND_SET:-missing}" "${QSTAR_COMMAND_STEP:-missing}" > "$output_path"
printf 'COMMAND-SET-OK\n'
