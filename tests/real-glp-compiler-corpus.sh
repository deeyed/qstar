#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
tmp=${TMPDIR:-/tmp}/qstar-real-glp-compiler-corpus.$$
ninja_available=0

case "$qstar" in
/*) ;;
*) qstar=$repo_dir/$qstar ;;
esac

if command -v ninja >/dev/null 2>&1; then
	ninja_available=1
fi

fail() {
	echo "qstar-real-glp-compiler-corpus: $*" >&2
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

copy_fixture() {
	language=$1
	backend=$2
	fixture=$repo_dir/tests/corpus/real-glp/$language-static-consumer
	project=$tmp/$language-$backend

	rm -rf "$project"
	mkdir -p "$(dirname "$project")"
	cp -R "$fixture" "$project"
	printf '%s\n' "$project"
}

run_backend() {
	language=$1
	backend=$2
	action_id=$3
	expected=$4
	compiler_argv=$5
	extra_argv_pattern=$6
	object_path=$7
	archive_path=$8
	exe_path=$9
	project=$(copy_fixture "$language" "$backend")
	out_prefix=$tmp/$language-$backend

	run_qstar "$project" check //... > "$out_prefix-check.out" 2> "$out_prefix-check.err"
	contains "$out_prefix-check.out" "status ok"
	run_qstar "$project" --dump-graph > "$out_prefix-graph.out" 2> "$out_prefix-graph.err"
	contains "$out_prefix-graph.out" "language_provider namespace=$language id=$language"
	contains "$out_prefix-graph.out" "sources [src/${language}_core."

	case "$backend" in
	stella)
		run_qstar "$project" build //:smoke > "$out_prefix-build.out" 2> "$out_prefix-build.err"
		;;
	ninja)
		if [ "$ninja_available" -eq 0 ]; then
			printf 'real_glp_compiler language=%s backend=ninja status=skipped reason=ninja-not-found\n' "$language"
			return 0
		fi
		run_qstar "$project" -G ninja build //:smoke > "$out_prefix-build.out" 2> "$out_prefix-build.err"
		contains "$out_prefix-build.out" "backend ninja"
		;;
	*)
		fail "unknown backend '$backend'"
		;;
	esac
	contains "$out_prefix-build.out" "run_expect label=//:smoke status=matched contains=$expected"
	contains "$out_prefix-build.out" "status ok"

	if [ "$backend" = ninja ]; then
		run_qstar "$project" -G ninja action-log "$action_id" \
			> "$out_prefix-action-log.out" 2> "$out_prefix-action-log.err"
	else
		run_qstar "$project" action-log "$action_id" \
			> "$out_prefix-action-log.out" 2> "$out_prefix-action-log.err"
	fi
	contains "$out_prefix-action-log.out" "argv[0]=$compiler_argv"
	contains "$out_prefix-action-log.out" "$extra_argv_pattern"
	contains "$out_prefix-action-log.out" "status ok"
	if [ "$language" = zig ]; then
		contains "$out_prefix-action-log.out" "envc=2"
		contains "$out_prefix-action-log.out" "env[0]=ZIG_GLOBAL_CACHE_DIR=<redacted>"
		contains "$out_prefix-action-log.out" "env[1]=ZIG_LOCAL_CACHE_DIR=<redacted>"
		not_contains "$out_prefix-action-log.out" "zig-global"
		if [ "$backend" = ninja ]; then
			test -d "$project/build/qstar/out/___zig_core/cache/zig-global" ||
				test -d "$project/build-ninja/qstar/out/___zig_core/cache/zig-global" ||
				fail "$language $backend provider cache dir missing"
		else
			test -d "$project/build/qstar/out/___zig_core/cache/zig-global" ||
				fail "$language $backend provider cache dir missing"
		fi
	else
		contains "$out_prefix-action-log.out" "envc=0"
	fi

	test -f "$project/$object_path" || fail "$language $backend object missing"
	test -f "$project/$archive_path" || fail "$language $backend archive missing"
	test -x "$project/$exe_path" || fail "$language $backend executable missing"
	"$project/$exe_path" > "$out_prefix-exe.out" 2> "$out_prefix-exe.err"
	contains "$out_prefix-exe.out" "$expected"

	printf 'real_glp_compiler language=%s backend=%s status=ok compiler=%s\n' \
		"$language" "$backend" "$compiler_argv"
}

run_language() {
	language=$1
	compiler=$2
	action_id=$3
	expected=$4
	compiler_argv=$5
	extra_argv_pattern=$6
	object_path=$7
	archive_path=$8
	exe_path=$9

	if ! command -v "$compiler" >/dev/null 2>&1; then
		printf 'real_glp_compiler language=%s status=skipped reason=compiler-not-found compiler=%s\n' \
			"$language" "$compiler"
		return 0
	fi

	run_backend "$language" stella "$action_id" "$expected" "$compiler_argv" \
		"$extra_argv_pattern" "$object_path" "$archive_path" "$exe_path"
	run_backend "$language" ninja "$action_id" "$expected" "$compiler_argv" \
		"$extra_argv_pattern" "$object_path" "$archive_path" "$exe_path"
}

rm -rf "$tmp"
mkdir -p "$tmp"
trap cleanup EXIT HUP INT TERM

case "$(uname -s 2>/dev/null || printf unknown)" in
MINGW*|MSYS*|CYGWIN*)
	printf 'real_glp_compiler_corpus status=skipped reason=windows-shell-fixture-deferred\n'
	exit 0
	;;
esac

run_language rust rustc //:rust_core:compile:0 rust-value=77 rustc \
	"--crate-type=lib" \
	build/qstar/out/___rust_core/obj0.o \
	build/qstar/out/___rust_core/librust_core.a \
	build/qstar/out/___consumer/consumer

run_language zig zig //:zig_core:compile:0 zig-value=88 zig \
	"build-obj" \
	build/qstar/out/___zig_core/obj0.o \
	build/qstar/out/___zig_core/libzig_core.a \
	build/qstar/out/___consumer/consumer

printf 'real_glp_compiler_corpus status=ok\n'
printf 'qstar-real-glp-compiler-corpus: passed\n'
