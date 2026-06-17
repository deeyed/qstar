#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
tmp=${TMPDIR:-/tmp}/qstar-real-language-init-scaffold.$$
ninja_available=0

case "$qstar" in
/*) ;;
*) qstar=$repo_dir/$qstar ;;
esac

if command -v ninja >/dev/null 2>&1; then
	ninja_available=1
fi

fail() {
	echo "qstar-real-language-init-scaffold: $*" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	if ! grep -F -q -- "$pattern" "$file"; then
		sed -n '1,160p' "$file" >&2 || true
		fail "missing pattern '$pattern' in $file"
	fi
}

not_contains() {
	file=$1
	pattern=$2
	if grep -F -q -- "$pattern" "$file"; then
		sed -n '1,160p' "$file" >&2 || true
		fail "unexpected pattern '$pattern' in $file"
	fi
}

cleanup() {
	rm -rf "$tmp"
}

run_qstar() {
	project=$1
	shift
	"$qstar" --file "$project/qstar.lua" "$@"
}

target_for_shape() {
	case "$1" in
	app)
		printf '%s\n' "//:app"
		;;
	lib)
		printf '%s\n' "//:core"
		;;
	tool)
		printf '%s\n' "//:tool"
		;;
	workspace)
		printf '%s\n' "//:all"
		;;
	*)
		fail "unknown init shape '$1'"
		;;
	esac
}

verify_artifacts() {
	project=$1
	shape=$2
	case "$shape" in
	app)
		test -x "$project/build/qstar/out/___app/app" ||
			fail "$project app executable missing"
		"$project/build/qstar/out/___app/app" >/dev/null
		;;
	lib)
		test -f "$project/build/qstar/out/___core/libcore.a" ||
			fail "$project core archive missing"
		;;
	tool)
		test -x "$project/build/qstar/out/___tool/tool" ||
			fail "$project tool executable missing"
		"$project/build/qstar/out/___tool/tool" >/dev/null
		;;
	workspace)
		test -f "$project/build/qstar/out/__packages_core_core/libcore.a" ||
			fail "$project workspace core archive missing"
		test -x "$project/build/qstar/out/__packages_app_app/app" ||
			fail "$project workspace app executable missing"
		"$project/build/qstar/out/__packages_app_app/app" >/dev/null
		;;
	esac
}

run_shape_backend() {
	language=$1
	backend=$2
	shape=$3
	project=$tmp/$language-$shape-$backend
	target=$(target_for_shape "$shape")
	out_prefix=$tmp/$language-$shape-$backend

	rm -rf "$project"
	"$qstar" init "$shape" "$project" --use-language="$language" \
		> "$out_prefix-init.out" 2> "$out_prefix-init.err"
	test -f "$project/qstar.lua" || fail "$language $shape qstar.lua missing"
	test -d "$project/qstar/languages/$language" ||
		fail "$language $shape provider was not vendored"
	run_qstar "$project" check //... > "$out_prefix-check.out" 2> "$out_prefix-check.err"
	contains "$out_prefix-check.out" "status ok"
	run_qstar "$project" --dump-graph > "$out_prefix-graph.out" 2> "$out_prefix-graph.err"
	contains "$out_prefix-graph.out" "language_provider namespace=$language id=$language"

	case "$backend" in
	stella)
		run_qstar "$project" build "$target" > "$out_prefix-build.out" 2> "$out_prefix-build.err"
		;;
	ninja)
		if [ "$ninja_available" -eq 0 ]; then
			printf 'real_language_init language=%s shape=%s backend=ninja status=skipped reason=ninja-not-found\n' \
				"$language" "$shape"
			return 0
		fi
		run_qstar "$project" -G ninja build "$target" > "$out_prefix-build.out" 2> "$out_prefix-build.err"
		contains "$out_prefix-build.out" "backend ninja"
		;;
	*)
		fail "unknown backend '$backend'"
		;;
	esac
	contains "$out_prefix-build.out" "status ok"
	not_contains "$out_prefix-build.out" "built for newer 'macOS' version"
	not_contains "$out_prefix-build.err" "built for newer 'macOS' version"
	verify_artifacts "$project" "$shape"

	printf 'real_language_init language=%s shape=%s backend=%s status=ok\n' \
		"$language" "$shape" "$backend"
}

run_language() {
	language=$1
	compiler=$2

	if ! command -v "$compiler" >/dev/null 2>&1; then
		printf 'real_language_init language=%s status=skipped reason=compiler-not-found compiler=%s\n' \
			"$language" "$compiler"
		return 0
	fi

	for shape in app lib tool workspace; do
		run_shape_backend "$language" stella "$shape"
		run_shape_backend "$language" ninja "$shape"
	done
}

rm -rf "$tmp"
mkdir -p "$tmp"
trap cleanup EXIT HUP INT TERM

case "$(uname -s 2>/dev/null || printf unknown)" in
MINGW*|MSYS*|CYGWIN*)
	printf 'real_language_init status=skipped reason=windows-shell-fixture-deferred\n'
	exit 0
	;;
esac

run_language rust rustc
run_language zig zig

printf 'real_language_init status=ok\n'
printf 'qstar-real-language-init-scaffold: passed\n'
