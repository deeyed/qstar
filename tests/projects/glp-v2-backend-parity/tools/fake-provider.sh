#!/bin/sh
set -eu

provider=
mode=
source=
object=
runtime=
metadata=
resources=
import_artifact=
depfile=
arg_log=${TMPDIR:-/tmp}/qstar-glp-v2-fake.$$.args

cleanup() {
  rm -f "$arg_log"
}
trap cleanup EXIT HUP INT TERM
: > "$arg_log"

parse_arg() {
  arg=$1
  printf '%s\n' "$arg" >> "$arg_log"
  case "$arg" in
    zig|rust|cuda)
      if [ -z "$provider" ]; then
        provider=$arg
      fi
      ;;
    compile|executable|archive|shared)
      mode=$arg
      ;;
    --source=*)
      source=${arg#--source=}
      ;;
    --object=*)
      object=${arg#--object=}
      ;;
    --runtime=*)
      runtime=${arg#--runtime=}
      ;;
    --metadata=*)
      metadata=${arg#--metadata=}
      ;;
    --resources=*)
      resources=${arg#--resources=}
      ;;
    --import=*)
      import_artifact=${arg#--import=}
      ;;
    --depfile=*)
      depfile=${arg#--depfile=}
      ;;
    source=*)
      if [ -z "$source" ]; then
        source=${arg#source=}
      fi
      ;;
  esac
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    @*)
      rsp=${1#@}
      while IFS= read -r arg; do
        parse_arg "$arg"
      done < "$rsp"
      ;;
    *)
      parse_arg "$1"
      ;;
  esac
  shift
done

test -n "$provider"
test -n "$mode"

if [ -n "$object" ]; then
  mkdir -p "$(dirname "$object")"
  printf 'provider=%s\nmode=%s\nsource=%s\n' "$provider" "$mode" "$source" > "$object"
fi

if [ -n "$runtime" ]; then
  mkdir -p "$(dirname "$runtime")"
  printf 'provider=%s\nmode=%s\nsource=%s\n' "$provider" "$mode" "$source" > "$runtime"
fi

if [ -n "$metadata" ]; then
  mkdir -p "$(dirname "$metadata")"
  {
    printf 'provider=%s\nmode=%s\nsource=%s\n' "$provider" "$mode" "$source"
    printf 'env=%s\n' "${QSTAR_GLP_V2_ENV:-<none>}"
    cat "$arg_log"
  } > "$metadata"
fi

if [ -n "$resources" ]; then
  mkdir -p "$resources"
  printf 'provider=%s\nmode=%s\n' "$provider" "$mode" > "$resources/index.txt"
fi

if [ -n "$import_artifact" ]; then
  mkdir -p "$(dirname "$import_artifact")"
  printf 'provider=%s\nmode=%s\nlink-interface=true\n' "$provider" "$mode" > "$import_artifact"
fi

if [ -n "$depfile" ]; then
  dep_target=$object
  if [ -z "$dep_target" ]; then
    dep_target=$runtime
  fi
  mkdir -p "$(dirname "$depfile")"
  printf '%s: src/tracked.input\n' "$dep_target" > "$depfile"
fi
