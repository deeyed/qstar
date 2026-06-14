#!/bin/sh
set -eu

tmp=${TMPDIR:-/tmp}/qstar-windows-artifacts-fake-clang-cl.$$
trap 'rm -f "$tmp"' EXIT HUP INT TERM

: > "$tmp"
for arg do
	case "$arg" in
	@*)
		sed 's/^"//; s/"$//' "${arg#@}" >> "$tmp"
		;;
	*)
		printf '%s\n' "$arg" >> "$tmp"
		;;
	esac
done

out=
dep=
expect=
while IFS= read -r line; do
	case "$line" in
	\"*\")
		clean=${line#\"}
		clean=${clean%\"}
		;;
	*)
		clean=$line
		;;
	esac
	if test "$expect" = out; then
		out=$clean
		expect=
		continue
	fi
	if test "$expect" = dep; then
		dep=$clean
		expect=
		continue
	fi
	case "$clean" in
	-o)
		expect=out
		;;
	-MF)
		expect=dep
		;;
	/out:*)
		out=${clean#/out:}
		;;
	esac
done < "$tmp"

test -n "$out" || {
	printf 'fake-clang-cl: output path not found\n' >&2
	exit 1
}

mkdir -p "$(dirname "$out")"
case "$out" in
*.o|*.obj)
	printf 'fake object\n' > "$out"
	if test -n "$dep"; then
		mkdir -p "$(dirname "$dep")"
		printf '%s: src/main.c\n' "$out" > "$dep"
	fi
	;;
*)
	printf '#!/bin/sh\nexit 0\n' > "$out"
	chmod +x "$out"
	;;
esac
