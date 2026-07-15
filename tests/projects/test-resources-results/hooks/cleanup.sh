#!/bin/sh
set -eu
printf 'cleanup %s\n' "$1" >> hook-events.log
