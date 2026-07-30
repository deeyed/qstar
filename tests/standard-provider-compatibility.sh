#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-standard-provider-compatibility.$$
last_step=setup
last_prefix=
dumped=0

dump_file_tail() {
	file=$1
	lines=${QSTAR_STANDARD_PROVIDER_TAIL_LINES:-80}
	if [ ! -f "$file" ]; then
		return 0
	fi
	echo "qstar-standard-provider-compatibility: --- $file (tail -n $lines) ---" >&2
	tail -n "$lines" "$file" >&2 || true
}

dump_related_file() {
	file=$1
	dump_file_tail "$file"
	case "$file" in
		*.out)
			dump_file_tail "${file%.out}.err"
			;;
		*.err)
			dump_file_tail "${file%.err}.out"
			;;
	esac
}

dump_step_files() {
	if [ "$dumped" -ne 0 ]; then
		return 0
	fi
	dumped=1
	if [ -n "$last_prefix" ]; then
		dump_related_file "$tmp/$last_prefix.out"
		dump_related_file "$tmp/$last_prefix.err"
	fi
}

fail() {
	echo "qstar-standard-provider-compatibility: $*" >&2
	dump_step_files
	exit 1
}

step() {
	last_step=$1
	last_prefix=${2:-}
}

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ]; then
		echo "qstar-standard-provider-compatibility: failed during $last_step (exit $rc)" >&2
		dump_step_files
	fi
	rm -rf "$tmp"
	exit "$rc"
}

contains() {
	file=$1
	pat=$2
	if ! grep -F -q -- "$pat" "$file"; then
		echo "qstar-standard-provider-compatibility: missing pattern '$pat' in $file" >&2
		dump_related_file "$file"
		dumped=1
		fail "missing pattern '$pat' in $file"
	fi
}

write_fake_zig_bin() {
	dir=$1
	mkdir -p "$dir"
	cat > "$dir/fake-zig" <<'EOF'
#!/bin/sh
set -eu

out=
src=
mode=

parse_arg() {
	case "$1" in
		build-obj|build-exe|build-lib)
			mode=$1
			;;
		-femit-bin=*)
			out=${1#-femit-bin=}
			;;
		*.zig)
			if [ -z "$src" ]; then
				src=$1
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

test -n "$out" || exit 2
mkdir -p "$(dirname "$out")"
tmp_c="$out.c"
case "$src" in
	*main.zig)
		printf 'int main(void) { return 0; }\n' > "$tmp_c"
		;;
	*)
		printf 'int qstar_fake_zig_value(void) { return 42; }\n' > "$tmp_c"
		;;
esac
mode=${mode:-build-obj}
case "$mode" in
	build-exe)
		"${QSTAR_FAKE_ZIG_CC:-${CC:-cc}}" -x c "$tmp_c" -o "$out"
		;;
	build-lib)
		tmp_o="$out.o"
		"${QSTAR_FAKE_ZIG_CC:-${CC:-cc}}" -x c -c "$tmp_c" -o "$tmp_o"
		"${AR:-ar}" rcs "$out" "$tmp_o"
		rm -f "$tmp_o"
		;;
	*)
		"${QSTAR_FAKE_ZIG_CC:-${CC:-cc}}" -x c -c "$tmp_c" -o "$out"
		;;
esac
rm -f "$tmp_c"
EOF
	chmod +x "$dir/fake-zig"
}

write_fake_rust_bin() {
	dir=$1
	mkdir -p "$dir"
	cat > "$dir/rustc" <<'EOF'
#!/bin/sh
set -eu

out=
src=
need_out=0
need_crate_type=0
crate_type=
emit_obj=0

parse_arg() {
	if [ "$need_out" -ne 0 ]; then
		out=$1
		need_out=0
		return
	fi
	if [ "$need_crate_type" -ne 0 ]; then
		crate_type=$1
		need_crate_type=0
		return
	fi
	case "$1" in
		-o)
			need_out=1
			;;
		--crate-type)
			need_crate_type=1
			;;
		--emit=obj)
			emit_obj=1
			;;
		*.rs)
			if [ -z "$src" ]; then
				src=$1
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

test -n "$out" || exit 2
mkdir -p "$(dirname "$out")"
tmp_c="$out.c"
case "$src" in
	*main.rs)
		printf 'int main(void) { return 0; }\n' > "$tmp_c"
		;;
	*)
		printf 'int qstar_fake_rust_value(void) { return 42; }\n' > "$tmp_c"
		;;
esac
case "$crate_type:$emit_obj" in
	bin:*)
		"${QSTAR_FAKE_RUST_CC:-${CC:-cc}}" -x c "$tmp_c" -o "$out"
		;;
	staticlib:*)
		tmp_o="$out.o"
		"${QSTAR_FAKE_RUST_CC:-${CC:-cc}}" -x c -c "$tmp_c" -o "$tmp_o"
		"${AR:-ar}" rcs "$out" "$tmp_o"
		rm -f "$tmp_o"
		;;
	*)
		"${QSTAR_FAKE_RUST_CC:-${CC:-cc}}" -x c -c "$tmp_c" -o "$out"
		;;
esac
rm -f "$tmp_c"
EOF
	chmod +x "$dir/rustc"
}

write_fake_cuda_bin() {
	dir=$1
	mkdir -p "$dir"
	cat > "$dir/nvcc" <<'EOF'
#!/bin/sh
set -eu

out=
src=
need_out=0

parse_arg() {
	if [ "$need_out" -ne 0 ]; then
		out=$1
		need_out=0
		return
	fi
	case "$1" in
		-o)
			need_out=1
			;;
		*.cu)
			if [ -z "$src" ]; then
				src=$1
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

test -n "$out" || exit 2
mkdir -p "$(dirname "$out")"
tmp_c="$out.c"
case "$src" in
	*main.cu)
		printf 'int main(void) { return 0; }\n' > "$tmp_c"
		;;
	*)
		printf 'int qstar_fake_cuda_value(void) { return 42; }\n' > "$tmp_c"
		;;
esac
"${QSTAR_FAKE_CUDA_CC:-${CC:-cc}}" -x c -c "$tmp_c" -o "$out"
rm -f "$tmp_c"
EOF
	chmod +x "$dir/nvcc"
}

check_standard_zig() {
	project=$tmp/standard-zig
	mkdir -p "$project/src" "$project/tools"
	write_fake_zig_bin "$project/tools"
	cat > "$project/src/main.zig" <<'EOF'
pub export fn standard_zig_value() i32 {
    return 7;
}
EOF
	cat > "$project/qstar.lua" <<'EOF'
local zig = qstar.use_language("zig")

qstar.project {
  name = "standard-zig",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    zig = zig.tools {
      compiler = qstar.cli {"tools/fake-zig"},
    },
  },
  response_files = "on",
  response_style = "posix",
}

qstar.config "zig_release" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      target = "aarch64-macos",
      optimize = "ReleaseFast",
      macos_min_version = "11.0",
      compile_options = {
        "--cache-dir",
        "build/zig-cache",
        "--standard-provider-compatibility",
        "--very-long-standard-provider-option-000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
        "--very-long-standard-provider-option-111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111",
      },
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:zig_release"},
  sources = {"src/main.zig"},
}
EOF
	test ! -d "$project/qstar/languages" ||
		fail "standard Zig fixture must not vendor qstar/languages"
	step "standard Zig check" standard-zig-check
	"$qstar" --file "$project/qstar.lua" check > "$tmp/standard-zig-check.out" 2> "$tmp/standard-zig-check.err"
	step "standard Zig graph" standard-zig-graph
	"$qstar" --file "$project/qstar.lua" --dump-graph > "$tmp/standard-zig-graph.out" 2> "$tmp/standard-zig-graph.err"
	contains "$tmp/standard-zig-graph.out" "language_provider namespace=zig id=zig api=qstar.lang/1 version=0.1"
	contains "$tmp/standard-zig-graph.out" "qstar/languages/zig/zig.qsm"
	contains "$tmp/standard-zig-graph.out" "tools.zig.compiler [tools/fake-zig]"
	step "standard Zig explain" standard-zig-explain
	"$qstar" --file "$project/qstar.lua" explain //:core > "$tmp/standard-zig-explain.out" 2> "$tmp/standard-zig-explain.err"
	contains "$tmp/standard-zig-explain.out" "source_file path=src/main.zig language=zig tool=provider-compiler provider=zig provider_role=compiler toolset_role=zig.compiler output_group=objects role=compile"
	contains "$tmp/standard-zig-explain.out" "build-lib"
	contains "$tmp/standard-zig-explain.out" "-static"
	contains "$tmp/standard-zig-explain.out" "-O, ReleaseFast"
	contains "$tmp/standard-zig-explain.out" "-target, aarch64-macos.11.0"
	contains "$tmp/standard-zig-explain.out" "-femit-bin=build/qstar/out/___core/libcore.a"
	step "standard Zig dry-run" standard-zig-dry-run
	"$qstar" --file "$project/qstar.lua" dry-run //:core > "$tmp/standard-zig-dry-run.out" 2> "$tmp/standard-zig-dry-run.err"
	contains "$tmp/standard-zig-dry-run.out" "dry_run_step id=//:core:archive:0 owner=//:core kind=archive tool=zig"
	contains "$tmp/standard-zig-dry-run.out" "argv=[tools/fake-zig, build-lib, src/main.zig"
	contains "$tmp/standard-zig-dry-run.out" "--standard-provider-compatibility"
	contains "$tmp/standard-zig-dry-run.out" "response=none"
	contains "$tmp/standard-zig-dry-run.out" "logical_argc=14"
	step "standard Zig build" standard-zig-build
	"$qstar" --file "$project/qstar.lua" build //:core > "$tmp/standard-zig-build.out" 2> "$tmp/standard-zig-build.err"
	contains "$tmp/standard-zig-build.out" "status ok"
	if ! find "$project/build" -name libcore.a -type f | grep -q .; then
		fail "standard Zig provider staticlib was not produced"
	fi
	step "standard Zig action-log" standard-zig-action-log
	"$qstar" --file "$project/qstar.lua" action-log //:core:archive:0 > "$tmp/standard-zig-action-log.out" 2> "$tmp/standard-zig-action-log.err"
	contains "$tmp/standard-zig-action-log.out" "tools/fake-zig"
	contains "$tmp/standard-zig-action-log.out" "build-lib"
	step "standard Zig replay" standard-zig-replay
	"$qstar" --file "$project/qstar.lua" replay //:core:archive:0 > "$tmp/standard-zig-replay.out" 2> "$tmp/standard-zig-replay.err"
	contains "$tmp/standard-zig-replay.out" "tools/fake-zig"
	contains "$tmp/standard-zig-replay.out" "-target aarch64-macos.11.0"
	step "standard Zig ninja build" standard-zig-ninja-build
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja build //:core > "$tmp/standard-zig-ninja-build.out" 2> "$tmp/standard-zig-ninja-build.err"
	contains "$tmp/standard-zig-ninja-build.out" "backend ninja"
	if ! find "$project/build-ninja" -name libcore.a -type f | grep -q .; then
		fail "standard Zig provider staticlib was not produced by Ninja"
	fi
	step "standard Zig ninja action-log" standard-zig-ninja-action-log
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja action-log //:core:archive:0 > "$tmp/standard-zig-ninja-action-log.out" 2> "$tmp/standard-zig-ninja-action-log.err"
	contains "$tmp/standard-zig-ninja-action-log.out" "backend=ninja"
	contains "$tmp/standard-zig-ninja-action-log.out" "tools/fake-zig"
	step "standard Zig ninja replay" standard-zig-ninja-replay
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja replay //:core:archive:0 > "$tmp/standard-zig-ninja-replay.out" 2> "$tmp/standard-zig-ninja-replay.err"
	contains "$tmp/standard-zig-ninja-replay.out" "tools/fake-zig"
	contains "$tmp/standard-zig-ninja-replay.out" "--standard-provider-compatibility"
}

check_standard_rust() {
	project=$tmp/standard-rust
	mkdir -p "$project/src" "$project/tools" "$project/vendor"
	write_fake_rust_bin "$project/tools"
	printf 'fake rlib\n' > "$project/vendor/libdep.rlib"
	cat > "$project/src/main.rs" <<'EOF'
#[no_mangle]
pub extern "C" fn standard_rust_value() -> i32 {
    7
}
EOF
	cat > "$project/qstar.lua" <<'EOF'
local rust = qstar.use_language("rust")

qstar.project {
  name = "standard-rust",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    rust = rust.tools {
      compiler = qstar.cli {"tools/rustc"},
    },
  },
}

qstar.config "rust_release" {
  toolset = "//:host",
  lang = {
    rust = rust.options {
      edition = "2021",
      crate_type = "lib",
      cfg = {"feature_demo"},
      externs = {"dep=vendor/libdep.rlib"},
      compile_options = {"-C", "panic=abort"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:rust_release"},
  sources = {"src/main.rs"},
}
EOF
	test ! -d "$project/qstar/languages" ||
		fail "standard Rust fixture must not vendor qstar/languages"
	step "standard Rust check" standard-rust-check
	"$qstar" --file "$project/qstar.lua" check > "$tmp/standard-rust-check.out" 2> "$tmp/standard-rust-check.err"
	step "standard Rust graph" standard-rust-graph
	"$qstar" --file "$project/qstar.lua" --dump-graph > "$tmp/standard-rust-graph.out" 2> "$tmp/standard-rust-graph.err"
	contains "$tmp/standard-rust-graph.out" "language_provider namespace=rust id=rust api=qstar.lang/1 version=0.1"
	contains "$tmp/standard-rust-graph.out" "qstar/languages/rust/rust.qsm"
	contains "$tmp/standard-rust-graph.out" "tools.rust.compiler [tools/rustc]"
	step "standard Rust explain" standard-rust-explain
	"$qstar" --file "$project/qstar.lua" explain //:core > "$tmp/standard-rust-explain.out" 2> "$tmp/standard-rust-explain.err"
	contains "$tmp/standard-rust-explain.out" "source_file path=src/main.rs language=rust tool=provider-compiler provider=rust provider_role=compiler toolset_role=rust.compiler output_group=objects role=compile"
	contains "$tmp/standard-rust-explain.out" "--edition, 2021"
	contains "$tmp/standard-rust-explain.out" "--crate-type, staticlib"
	contains "$tmp/standard-rust-explain.out" "--cfg, feature_demo"
	contains "$tmp/standard-rust-explain.out" "--extern, dep=vendor/libdep.rlib"
	contains "$tmp/standard-rust-explain.out" "-C, panic=abort"
	contains "$tmp/standard-rust-explain.out" "-o, build/qstar/out/___core/libcore.a"
	step "standard Rust dry-run" standard-rust-dry-run
	"$qstar" --file "$project/qstar.lua" dry-run //:core > "$tmp/standard-rust-dry-run.out" 2> "$tmp/standard-rust-dry-run.err"
	contains "$tmp/standard-rust-dry-run.out" "dry_run_step id=//:core:archive:0 owner=//:core kind=archive tool=rust"
	contains "$tmp/standard-rust-dry-run.out" "argv=[tools/rustc, --edition, 2021, --crate-type, staticlib, src/main.rs"
	contains "$tmp/standard-rust-dry-run.out" "--extern, dep=vendor/libdep.rlib"
	step "standard Rust build" standard-rust-build
	"$qstar" --file "$project/qstar.lua" build //:core > "$tmp/standard-rust-build.out" 2> "$tmp/standard-rust-build.err"
	contains "$tmp/standard-rust-build.out" "status ok"
	if ! find "$project/build" -name libcore.a -type f | grep -q .; then
		fail "standard Rust provider staticlib was not produced"
	fi
	step "standard Rust action-log" standard-rust-action-log
	"$qstar" --file "$project/qstar.lua" action-log //:core:archive:0 > "$tmp/standard-rust-action-log.out" 2> "$tmp/standard-rust-action-log.err"
	contains "$tmp/standard-rust-action-log.out" "tools/rustc"
	contains "$tmp/standard-rust-action-log.out" "--crate-type"
	step "standard Rust replay" standard-rust-replay
	"$qstar" --file "$project/qstar.lua" replay //:core:archive:0 > "$tmp/standard-rust-replay.out" 2> "$tmp/standard-rust-replay.err"
	contains "$tmp/standard-rust-replay.out" "tools/rustc"
	contains "$tmp/standard-rust-replay.out" "--cfg feature_demo"
	step "standard Rust ninja build" standard-rust-ninja-build
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja build //:core > "$tmp/standard-rust-ninja-build.out" 2> "$tmp/standard-rust-ninja-build.err"
	contains "$tmp/standard-rust-ninja-build.out" "backend ninja"
	if ! find "$project/build-ninja" -name libcore.a -type f | grep -q .; then
		fail "standard Rust provider staticlib was not produced by Ninja"
	fi
	step "standard Rust ninja action-log" standard-rust-ninja-action-log
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja action-log //:core:archive:0 > "$tmp/standard-rust-ninja-action-log.out" 2> "$tmp/standard-rust-ninja-action-log.err"
	contains "$tmp/standard-rust-ninja-action-log.out" "backend=ninja"
	contains "$tmp/standard-rust-ninja-action-log.out" "tools/rustc"
	step "standard Rust ninja replay" standard-rust-ninja-replay
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja replay //:core:archive:0 > "$tmp/standard-rust-ninja-replay.out" 2> "$tmp/standard-rust-ninja-replay.err"
	contains "$tmp/standard-rust-ninja-replay.out" "tools/rustc"
	contains "$tmp/standard-rust-ninja-replay.out" "--extern dep=vendor/libdep.rlib"
}

check_standard_cuda() {
	project=$tmp/standard-cuda
	mkdir -p "$project/src" "$project/tools"
	write_fake_cuda_bin "$project/tools"
	cat > "$project/src/main.cu" <<'EOF'
extern "C" int standard_cuda_value() {
    return 7;
}
EOF
	cat > "$project/qstar.lua" <<'EOF'
local cuda = qstar.use_language("cuda")

qstar.project {
  name = "standard-cuda",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    cuda = cuda.tools {
      compiler = qstar.cli {"tools/nvcc"},
    },
  },
}

qstar.config "cuda_release" {
  toolset = "//:host",
  lang = {
    cuda = cuda.options {
      arch = "sm_70",
      compile_options = {"--fake-device-option"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:cuda_release"},
  sources = {"src/main.cu"},
}
EOF
	test ! -d "$project/qstar/languages" ||
		fail "standard CUDA fixture must not vendor qstar/languages"
	step "standard CUDA check" standard-cuda-check
	"$qstar" --file "$project/qstar.lua" check > "$tmp/standard-cuda-check.out" 2> "$tmp/standard-cuda-check.err"
	step "standard CUDA graph" standard-cuda-graph
	"$qstar" --file "$project/qstar.lua" --dump-graph > "$tmp/standard-cuda-graph.out" 2> "$tmp/standard-cuda-graph.err"
	contains "$tmp/standard-cuda-graph.out" "language_provider namespace=cuda id=cuda api=qstar.lang/1 version=0.1"
	contains "$tmp/standard-cuda-graph.out" "qstar/languages/cuda/cuda.qsm"
	contains "$tmp/standard-cuda-graph.out" "tools.cuda.compiler [tools/nvcc]"
	step "standard CUDA explain" standard-cuda-explain
	"$qstar" --file "$project/qstar.lua" explain //:core > "$tmp/standard-cuda-explain.out" 2> "$tmp/standard-cuda-explain.err"
	contains "$tmp/standard-cuda-explain.out" "source_file path=src/main.cu language=cuda tool=provider-compiler provider=cuda provider_role=compiler toolset_role=cuda.compiler output_group=objects role=compile"
	contains "$tmp/standard-cuda-explain.out" "-arch, sm_70"
	contains "$tmp/standard-cuda-explain.out" "-c, src/main.cu"
	contains "$tmp/standard-cuda-explain.out" "-o, build/qstar/out/___core/obj0.o"
	contains "$tmp/standard-cuda-explain.out" "--fake-device-option"
	step "standard CUDA dry-run" standard-cuda-dry-run
	"$qstar" --file "$project/qstar.lua" dry-run //:core > "$tmp/standard-cuda-dry-run.out" 2> "$tmp/standard-cuda-dry-run.err"
	contains "$tmp/standard-cuda-dry-run.out" "dry_run_step id=//:core:compile:0 owner=//:core kind=compile language=cuda tool=provider-compiler"
	contains "$tmp/standard-cuda-dry-run.out" "argv=[tools/nvcc, -arch, sm_70, -c, src/main.cu"
	step "standard CUDA build" standard-cuda-build
	"$qstar" --file "$project/qstar.lua" build //:core > "$tmp/standard-cuda-build.out" 2> "$tmp/standard-cuda-build.err"
	contains "$tmp/standard-cuda-build.out" "status ok"
	if ! find "$project/build" -name obj0.o -type f | grep -q .; then
		fail "standard CUDA provider object was not produced"
	fi
	step "standard CUDA action-log" standard-cuda-action-log
	"$qstar" --file "$project/qstar.lua" action-log //:core:compile:0 > "$tmp/standard-cuda-action-log.out" 2> "$tmp/standard-cuda-action-log.err"
	contains "$tmp/standard-cuda-action-log.out" "tools/nvcc"
	contains "$tmp/standard-cuda-action-log.out" "-arch"
	step "standard CUDA replay" standard-cuda-replay
	"$qstar" --file "$project/qstar.lua" replay //:core:compile:0 > "$tmp/standard-cuda-replay.out" 2> "$tmp/standard-cuda-replay.err"
	contains "$tmp/standard-cuda-replay.out" "tools/nvcc"
	contains "$tmp/standard-cuda-replay.out" "-arch sm_70"
	step "standard CUDA ninja build" standard-cuda-ninja-build
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja build //:core > "$tmp/standard-cuda-ninja-build.out" 2> "$tmp/standard-cuda-ninja-build.err"
	contains "$tmp/standard-cuda-ninja-build.out" "backend ninja"
	if ! find "$project/build-ninja" -name obj0.o -type f | grep -q .; then
		fail "standard CUDA provider object was not produced by Ninja"
	fi
	step "standard CUDA ninja action-log" standard-cuda-ninja-action-log
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja action-log //:core:compile:0 > "$tmp/standard-cuda-ninja-action-log.out" 2> "$tmp/standard-cuda-ninja-action-log.err"
	contains "$tmp/standard-cuda-ninja-action-log.out" "backend=ninja"
	contains "$tmp/standard-cuda-ninja-action-log.out" "tools/nvcc"
	step "standard CUDA ninja replay" standard-cuda-ninja-replay
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja replay //:core:compile:0 > "$tmp/standard-cuda-ninja-replay.out" 2> "$tmp/standard-cuda-ninja-replay.err"
	contains "$tmp/standard-cuda-ninja-replay.out" "tools/nvcc"
	contains "$tmp/standard-cuda-ninja-replay.out" "--fake-device-option"
}

check_standard_provider_objectlibs() {
	project=$tmp/standard-provider-objectlibs
	mkdir -p "$project/src" "$project/tools"
	write_fake_zig_bin "$project/tools"
	write_fake_rust_bin "$project/tools"
	write_fake_cuda_bin "$project/tools"
	cat > "$project/src/zig_leaf.zig" <<'EOF'
pub export fn qstar_fake_zig_value() i32 {
    return 42;
}
EOF
	cat > "$project/src/rust_leaf.rs" <<'EOF'
#[no_mangle]
pub extern "C" fn qstar_fake_rust_value() -> i32 {
    42
}
EOF
	cat > "$project/src/cuda_leaf.cu" <<'EOF'
extern "C" int qstar_fake_cuda_value() {
    return 42;
}
EOF
	cat > "$project/qstar.lua" <<'EOF'
local zig = qstar.use_language("zig")
local rust = qstar.use_language("rust")
local cuda = qstar.use_language("cuda")

qstar.project {
  name = "standard-provider-objectlibs",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    zig = zig.tools {
      compiler = qstar.cli {"tools/fake-zig"},
    },
    rust = rust.tools {
      compiler = qstar.cli {"tools/rustc"},
    },
    cuda = cuda.tools {
      compiler = qstar.cli {"tools/nvcc"},
    },
  },
}

qstar.config "providers" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      optimize = "ReleaseSafe",
      compile_options = {"--objectlib-raw-zig"},
    },
    rust = rust.options {
      edition = "2021",
      crate_type = "lib",
    },
    cuda = cuda.options {
      arch = "sm_70",
      compile_options = {"--objectlib-raw-cuda"},
    },
  },
}

qstar.objectlib "zig_objects" {
  configs = {"//:providers"},
  sources = {"src/zig_leaf.zig"},
}

qstar.objectlib "rust_objects" {
  configs = {"//:providers"},
  sources = {
    rust.object("src/rust_leaf.rs", {
      cfg = {"objectlib_token"},
      compile_options = {"-C", "panic=abort"},
    }),
  },
}

qstar.objectlib "cuda_objects" {
  configs = {"//:providers"},
  sources = {
    cuda.object("src/cuda_leaf.cu", {
      compile_options = {"--objectlib-token-cuda"},
    }),
  },
}

qstar.staticlib "zig_pack" {
  configs = {"//:providers"},
  objects = {"//:zig_objects"},
}

qstar.staticlib "rust_pack" {
  configs = {"//:providers"},
  objects = {"//:rust_objects"},
}

qstar.staticlib "cuda_pack" {
  configs = {"//:providers"},
  objects = {"//:cuda_objects"},
}

qstar.group "all" {
  deps = {"//:zig_pack", "//:rust_pack", "//:cuda_pack"},
}
EOF
	step "standard provider objectlib check" standard-provider-objectlib-check
	"$qstar" --file "$project/qstar.lua" check > "$tmp/standard-provider-objectlib-check.out" 2> "$tmp/standard-provider-objectlib-check.err"
	step "standard provider objectlib list-targets" standard-provider-objectlib-list-targets
	"$qstar" --file "$project/qstar.lua" list-targets --format json > "$tmp/standard-provider-objectlib-list-targets.out" 2> "$tmp/standard-provider-objectlib-list-targets.err"
	contains "$tmp/standard-provider-objectlib-list-targets.out" "\"language_provider_count\":3"
	contains "$tmp/standard-provider-objectlib-list-targets.out" "\"label\":\"//:zig_objects\""
	contains "$tmp/standard-provider-objectlib-list-targets.out" "\"kind\":\"objectlib\""
	contains "$tmp/standard-provider-objectlib-list-targets.out" "\"sources\":[\"src/zig_leaf.zig\"]"
	contains "$tmp/standard-provider-objectlib-list-targets.out" "\"compile_context\":\"own\""
	contains "$tmp/standard-provider-objectlib-list-targets.out" "\"label\":\"//:zig_pack\""
	contains "$tmp/standard-provider-objectlib-list-targets.out" "\"objects\":[\"//:zig_objects\"]"
	step "standard provider objectlib query" standard-provider-objectlib-query
	"$qstar" --file "$project/qstar.lua" query //:zig_objects > "$tmp/standard-provider-objectlib-query.out" 2> "$tmp/standard-provider-objectlib-query.err"
	contains "$tmp/standard-provider-objectlib-query.out" "kind objectlib"
	contains "$tmp/standard-provider-objectlib-query.out" "sources [src/zig_leaf.zig]"
	contains "$tmp/standard-provider-objectlib-query.out" "compile_context own"
	"$qstar" --file "$project/qstar.lua" query //:zig_pack > "$tmp/standard-provider-objectlib-pack-query.out" 2> "$tmp/standard-provider-objectlib-pack-query.err"
	contains "$tmp/standard-provider-objectlib-pack-query.out" "kind staticlib"
	contains "$tmp/standard-provider-objectlib-pack-query.out" "objects [//:zig_objects]"
	step "standard provider objectlib explain" standard-provider-objectlib-explain
	"$qstar" --file "$project/qstar.lua" explain //:all > "$tmp/standard-provider-objectlib-explain.out" 2> "$tmp/standard-provider-objectlib-explain.err"
	contains "$tmp/standard-provider-objectlib-explain.out" "target //:zig_objects"
	contains "$tmp/standard-provider-objectlib-explain.out" "source_file path=src/zig_leaf.zig language=zig tool=provider-compiler"
	contains "$tmp/standard-provider-objectlib-explain.out" "target //:rust_objects"
	contains "$tmp/standard-provider-objectlib-explain.out" "source_file path=src/rust_leaf.rs language=rust tool=provider-compiler"
	contains "$tmp/standard-provider-objectlib-explain.out" "target //:cuda_objects"
	contains "$tmp/standard-provider-objectlib-explain.out" "source_file path=src/cuda_leaf.cu language=cuda tool=provider-compiler"
	step "standard provider objectlib dry-run" standard-provider-objectlib-dry-run
	"$qstar" --file "$project/qstar.lua" dry-run //:all > "$tmp/standard-provider-objectlib-dry-run.out" 2> "$tmp/standard-provider-objectlib-dry-run.err"
	contains "$tmp/standard-provider-objectlib-dry-run.out" "dry_run_step id=//:zig_objects:compile:0 owner=//:zig_objects kind=compile language=zig"
	contains "$tmp/standard-provider-objectlib-dry-run.out" "dry_run_step id=//:rust_objects:compile:0 owner=//:rust_objects kind=compile language=rust"
	contains "$tmp/standard-provider-objectlib-dry-run.out" "dry_run_step id=//:cuda_objects:compile:0 owner=//:cuda_objects kind=compile language=cuda"
	contains "$tmp/standard-provider-objectlib-dry-run.out" "dry_run_step id=//:zig_objects:compile-objects:0 owner=//:zig_objects kind=compile-objects"
	step "standard provider objectlib build" standard-provider-objectlib-build
	"$qstar" --file "$project/qstar.lua" build //:all > "$tmp/standard-provider-objectlib-build.out" 2> "$tmp/standard-provider-objectlib-build.err"
	contains "$tmp/standard-provider-objectlib-build.out" "status ok"
	test -f "$project/build/qstar/out/___zig_objects/obj0.o" ||
		fail "standard Zig objectlib object missing"
	test -f "$project/build/qstar/out/___rust_objects/obj0.o" ||
		fail "standard Rust objectlib object missing"
	test -f "$project/build/qstar/out/___cuda_objects/obj0.o" ||
		fail "standard CUDA objectlib object missing"
	test -f "$project/build/qstar/out/___zig_pack/libzig_pack.a" ||
		fail "standard Zig objectlib archive missing"
	test -f "$project/build/qstar/out/___rust_pack/librust_pack.a" ||
		fail "standard Rust objectlib archive missing"
	test -f "$project/build/qstar/out/___cuda_pack/libcuda_pack.a" ||
		fail "standard CUDA objectlib archive missing"
	contains "$project/build/qstar/compile_commands.json" "src/zig_leaf.zig"
	contains "$project/build/qstar/compile_commands.json" "src/rust_leaf.rs"
	contains "$project/build/qstar/compile_commands.json" "src/cuda_leaf.cu"
	contains "$project/build/qstar/compile_commands.json" "build/qstar/out/___zig_objects/obj0.o"
	contains "$project/build/qstar/compile_commands.json" "build/qstar/out/___rust_objects/obj0.o"
	contains "$project/build/qstar/compile_commands.json" "build/qstar/out/___cuda_objects/obj0.o"
	step "standard provider objectlib why-rebuild" standard-provider-objectlib-why-rebuild
	"$qstar" --file "$project/qstar.lua" why-rebuild //:zig_objects > "$tmp/standard-provider-objectlib-why-rebuild.out" 2> "$tmp/standard-provider-objectlib-why-rebuild.err"
	contains "$tmp/standard-provider-objectlib-why-rebuild.out" "qstar why-rebuild v1"
	contains "$tmp/standard-provider-objectlib-why-rebuild.out" "root //:zig_objects"
	contains "$tmp/standard-provider-objectlib-why-rebuild.out" "reason=output-check"
	contains "$tmp/standard-provider-objectlib-why-rebuild.out" "status=skip"
	step "standard provider objectlib action-log" standard-provider-objectlib-action-log
	"$qstar" --file "$project/qstar.lua" action-log //:zig_objects:compile:0 > "$tmp/standard-provider-objectlib-zig-log.out" 2> "$tmp/standard-provider-objectlib-zig-log.err"
	contains "$tmp/standard-provider-objectlib-zig-log.out" "action //:zig_objects:compile:0"
	contains "$tmp/standard-provider-objectlib-zig-log.out" "backend=stella"
	contains "$tmp/standard-provider-objectlib-zig-log.out" "tools/fake-zig"
	contains "$tmp/standard-provider-objectlib-zig-log.out" "build-obj"
	contains "$tmp/standard-provider-objectlib-zig-log.out" "--objectlib-raw-zig"
	"$qstar" --file "$project/qstar.lua" action-log //:rust_objects:compile:0 > "$tmp/standard-provider-objectlib-rust-log.out" 2> "$tmp/standard-provider-objectlib-rust-log.err"
	contains "$tmp/standard-provider-objectlib-rust-log.out" "action //:rust_objects:compile:0"
	contains "$tmp/standard-provider-objectlib-rust-log.out" "backend=stella"
	contains "$tmp/standard-provider-objectlib-rust-log.out" "tools/rustc"
	contains "$tmp/standard-provider-objectlib-rust-log.out" "--emit=obj"
	contains "$tmp/standard-provider-objectlib-rust-log.out" "objectlib_token"
	"$qstar" --file "$project/qstar.lua" action-log //:cuda_objects:compile:0 > "$tmp/standard-provider-objectlib-cuda-log.out" 2> "$tmp/standard-provider-objectlib-cuda-log.err"
	contains "$tmp/standard-provider-objectlib-cuda-log.out" "action //:cuda_objects:compile:0"
	contains "$tmp/standard-provider-objectlib-cuda-log.out" "backend=stella"
	contains "$tmp/standard-provider-objectlib-cuda-log.out" "tools/nvcc"
	contains "$tmp/standard-provider-objectlib-cuda-log.out" "--objectlib-token-cuda"
	step "standard provider objectlib replay" standard-provider-objectlib-replay
	"$qstar" --file "$project/qstar.lua" replay //:zig_objects:compile:0 > "$tmp/standard-provider-objectlib-zig-replay.out" 2> "$tmp/standard-provider-objectlib-zig-replay.err"
	contains "$tmp/standard-provider-objectlib-zig-replay.out" "qstar replay v1"
	contains "$tmp/standard-provider-objectlib-zig-replay.out" "tools/fake-zig"
	contains "$tmp/standard-provider-objectlib-zig-replay.out" "--objectlib-raw-zig"
	"$qstar" --file "$project/qstar.lua" replay //:rust_objects:compile:0 > "$tmp/standard-provider-objectlib-rust-replay.out" 2> "$tmp/standard-provider-objectlib-rust-replay.err"
	contains "$tmp/standard-provider-objectlib-rust-replay.out" "qstar replay v1"
	contains "$tmp/standard-provider-objectlib-rust-replay.out" "tools/rustc"
	contains "$tmp/standard-provider-objectlib-rust-replay.out" "objectlib_token"
	"$qstar" --file "$project/qstar.lua" replay //:cuda_objects:compile:0 > "$tmp/standard-provider-objectlib-cuda-replay.out" 2> "$tmp/standard-provider-objectlib-cuda-replay.err"
	contains "$tmp/standard-provider-objectlib-cuda-replay.out" "qstar replay v1"
	contains "$tmp/standard-provider-objectlib-cuda-replay.out" "tools/nvcc"
	contains "$tmp/standard-provider-objectlib-cuda-replay.out" "--objectlib-token-cuda"
	step "standard provider objectlib ninja" standard-provider-objectlib-ninja
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja build //:all > "$tmp/standard-provider-objectlib-ninja.out" 2> "$tmp/standard-provider-objectlib-ninja.err"
	contains "$tmp/standard-provider-objectlib-ninja.out" "backend ninja"
	test -f "$project/build-ninja/out/___zig_objects/obj0.o" ||
		fail "standard Zig objectlib Ninja object missing"
	test -f "$project/build-ninja/out/___rust_objects/obj0.o" ||
		fail "standard Rust objectlib Ninja object missing"
	test -f "$project/build-ninja/out/___cuda_objects/obj0.o" ||
		fail "standard CUDA objectlib Ninja object missing"
	test -f "$project/build-ninja/out/___zig_pack/libzig_pack.a" ||
		fail "standard Zig objectlib Ninja archive missing"
	test -f "$project/build-ninja/out/___rust_pack/librust_pack.a" ||
		fail "standard Rust objectlib Ninja archive missing"
	test -f "$project/build-ninja/out/___cuda_pack/libcuda_pack.a" ||
		fail "standard CUDA objectlib Ninja archive missing"
	contains "$project/build-ninja/ninja/build.ninja" "qstar_action_id = //:zig_objects:compile:0"
	contains "$project/build-ninja/ninja/build.ninja" "qstar_action_id = //:rust_objects:compile:0"
	contains "$project/build-ninja/ninja/build.ninja" "qstar_action_id = //:cuda_objects:compile:0"
	contains "$project/build-ninja/compile_commands.json" "src/zig_leaf.zig"
	contains "$project/build-ninja/compile_commands.json" "src/rust_leaf.rs"
	contains "$project/build-ninja/compile_commands.json" "src/cuda_leaf.cu"
	contains "$project/build-ninja/compile_commands.json" "build-ninja/out/___zig_objects/obj0.o"
	contains "$project/build-ninja/compile_commands.json" "build-ninja/out/___rust_objects/obj0.o"
	contains "$project/build-ninja/compile_commands.json" "build-ninja/out/___cuda_objects/obj0.o"
	step "standard provider objectlib ninja action-log" standard-provider-objectlib-ninja-action-log
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja action-log //:zig_objects:compile:0 > "$tmp/standard-provider-objectlib-ninja-zig-log.out" 2> "$tmp/standard-provider-objectlib-ninja-zig-log.err"
	contains "$tmp/standard-provider-objectlib-ninja-zig-log.out" "backend=ninja"
	contains "$tmp/standard-provider-objectlib-ninja-zig-log.out" "tools/fake-zig"
	contains "$tmp/standard-provider-objectlib-ninja-zig-log.out" "--objectlib-raw-zig"
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja action-log //:rust_objects:compile:0 > "$tmp/standard-provider-objectlib-ninja-rust-log.out" 2> "$tmp/standard-provider-objectlib-ninja-rust-log.err"
	contains "$tmp/standard-provider-objectlib-ninja-rust-log.out" "backend=ninja"
	contains "$tmp/standard-provider-objectlib-ninja-rust-log.out" "tools/rustc"
	contains "$tmp/standard-provider-objectlib-ninja-rust-log.out" "objectlib_token"
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja action-log //:cuda_objects:compile:0 > "$tmp/standard-provider-objectlib-ninja-cuda-log.out" 2> "$tmp/standard-provider-objectlib-ninja-cuda-log.err"
	contains "$tmp/standard-provider-objectlib-ninja-cuda-log.out" "backend=ninja"
	contains "$tmp/standard-provider-objectlib-ninja-cuda-log.out" "tools/nvcc"
	contains "$tmp/standard-provider-objectlib-ninja-cuda-log.out" "--objectlib-token-cuda"
	step "standard provider objectlib ninja replay" standard-provider-objectlib-ninja-replay
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja replay //:zig_objects:compile:0 > "$tmp/standard-provider-objectlib-ninja-zig-replay.out" 2> "$tmp/standard-provider-objectlib-ninja-zig-replay.err"
	contains "$tmp/standard-provider-objectlib-ninja-zig-replay.out" "tools/fake-zig"
	contains "$tmp/standard-provider-objectlib-ninja-zig-replay.out" "--objectlib-raw-zig"
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja replay //:rust_objects:compile:0 > "$tmp/standard-provider-objectlib-ninja-rust-replay.out" 2> "$tmp/standard-provider-objectlib-ninja-rust-replay.err"
	contains "$tmp/standard-provider-objectlib-ninja-rust-replay.out" "tools/rustc"
	contains "$tmp/standard-provider-objectlib-ninja-rust-replay.out" "objectlib_token"
	"$qstar" --file "$project/qstar.lua" -B build-ninja -G ninja replay //:cuda_objects:compile:0 > "$tmp/standard-provider-objectlib-ninja-replay.out" 2> "$tmp/standard-provider-objectlib-ninja-replay.err"
	contains "$tmp/standard-provider-objectlib-ninja-replay.out" "tools/nvcc"
	contains "$tmp/standard-provider-objectlib-ninja-replay.out" "--objectlib-token-cuda"
}

check_standard_provider_objectlib_diagnostics() {
	project=$tmp/standard-provider-objectlib-diagnostics
	mkdir -p "$project/src" "$project/tools"
	write_fake_zig_bin "$project/tools"
	cat > "$project/src/leaf.zig" <<'EOF'
pub export fn qstar_fake_zig_value() i32 {
    return 42;
}
EOF
	cat > "$project/qstar.lua" <<'EOF'
local zig = qstar.use_language("zig")

qstar.project {
  name = "standard-provider-objectlib-diagnostics",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    zig = zig.tools {
      compiler = qstar.cli {"tools/fake-zig"},
    },
  },
}

qstar.config "providers" {
  toolset = "//:host",
}

qstar.objectlib "bad_objects" {
  compile_context = "consumer",
  configs = {"//:providers"},
  sources = {
    zig.object("src/leaf.zig"),
  },
}

qstar.staticlib "pack" {
  configs = {"//:providers"},
  objects = {"//:bad_objects"},
}
EOF
	step "standard provider objectlib diagnostic" standard-provider-objectlib-diagnostic
	if "$qstar" --file "$project/qstar.lua" dry-run //:pack > "$tmp/standard-provider-objectlib-diagnostic.out" 2> "$tmp/standard-provider-objectlib-diagnostic.err"; then
		fail "consumer-context provider source token unexpectedly succeeded"
	fi
	contains "$tmp/standard-provider-objectlib-diagnostic.err" "provider source token"
	contains "$tmp/standard-provider-objectlib-diagnostic.err" "compile_context = \"consumer\" objectlib"
}

check_standard_option_schema() {
	project=$tmp/standard-provider-options
	mkdir -p "$project"

	cat > "$project/qstar.lua" <<'EOF'
local zig = qstar.use_language("zig")

qstar.config "bad" {
  lang = {
    zig = zig.options {
      optimize = "ReleaseSlow",
    },
  },
}
EOF
	step "standard Zig option schema" standard-zig-option-enum
	if "$qstar" --file "$project/qstar.lua" check > "$tmp/standard-zig-option-enum.out" 2> "$tmp/standard-zig-option-enum.err"; then
		fail "bad standard Zig enum option unexpectedly succeeded"
	fi
	contains "$tmp/standard-zig-option-enum.err" "lang.zig.optimize has unsupported enum value 'ReleaseSlow'"

	cat > "$project/qstar.lua" <<'EOF'
local rust = qstar.use_language("rust")

qstar.config "bad" {
  lang = {
    rust = rust.options {
      crate_type = "bundle",
    },
  },
}
EOF
	step "standard Rust option schema" standard-rust-option-enum
	if "$qstar" --file "$project/qstar.lua" check > "$tmp/standard-rust-option-enum.out" 2> "$tmp/standard-rust-option-enum.err"; then
		fail "bad standard Rust enum option unexpectedly succeeded"
	fi
	contains "$tmp/standard-rust-option-enum.err" "lang.rust.crate_type has unsupported enum value 'bundle'"

	cat > "$project/qstar.lua" <<'EOF'
local cuda = qstar.use_language("cuda")

qstar.config "bad" {
  lang = {
    cuda = cuda.options {
      arch = false,
    },
  },
}
EOF
	step "standard CUDA option schema" standard-cuda-option-string
	if "$qstar" --file "$project/qstar.lua" check > "$tmp/standard-cuda-option-string.out" 2> "$tmp/standard-cuda-option-string.err"; then
		fail "bad standard CUDA string option unexpectedly succeeded"
	fi
	contains "$tmp/standard-cuda-option-string.err" "lang.cuda.arch must be a string"

	cat > "$project/qstar.lua" <<'EOF'
local cuda = qstar.use_language("cuda")

qstar.config "bad" {
  lang = {
    cuda = cuda.options {
      compile_options = {"--ok", true},
    },
  },
}
EOF
	step "standard CUDA list schema" standard-cuda-option-list
	if "$qstar" --file "$project/qstar.lua" check > "$tmp/standard-cuda-option-list.out" 2> "$tmp/standard-cuda-option-list.err"; then
		fail "bad standard CUDA list option unexpectedly succeeded"
	fi
	contains "$tmp/standard-cuda-option-list.err" "lang.cuda.compile_options must be a list of strings"
}

rm -rf "$tmp"
mkdir -p "$tmp"
trap cleanup EXIT HUP INT TERM

command -v ninja >/dev/null 2>&1 ||
	fail "ninja is required for standard provider compatibility parity"

step "standard Zig provider" standard-zig
check_standard_zig
step "standard Rust provider" standard-rust
check_standard_rust
step "standard CUDA provider" standard-cuda
check_standard_cuda
step "standard provider objectlibs" standard-provider-objectlibs
check_standard_provider_objectlibs
step "standard provider objectlib diagnostics" standard-provider-objectlib-diagnostics
check_standard_provider_objectlib_diagnostics
step "standard provider option schema" standard-provider-options
check_standard_option_schema

printf '%s\n' "qstar-standard-provider-compatibility: passed"
