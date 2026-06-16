#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-linux-validation.$$
install_root="$tmp/install-root"
project="$tmp/project"
host=$(uname -s 2>/dev/null || printf unknown)
validation_cc=${QSTAR_LINUX_VALIDATION_CC:-cc}

fail() {
	printf 'qstar-linux-validation: %s\n' "$1" >&2
	exit 1
}

contains() {
	file=$1
	pattern=$2
	grep -F -q -- "$pattern" "$file" ||
		fail "missing pattern '$pattern' in $file"
}

not_contains() {
	file=$1
	pattern=$2
	if grep -F -q -- "$pattern" "$file"; then
		fail "unexpected pattern '$pattern' in $file"
	fi
}

write_fake_zig_bin() {
	dir=$1
	mkdir -p "$dir"
	cat > "$dir/zig" <<'EOF'
#!/bin/sh
set -eu

out=
src=

parse_arg() {
	case "$1" in
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
"${QSTAR_FAKE_ZIG_CC:-cc}" -x c -c "$tmp_c" -o "$out"
rm -f "$tmp_c"
EOF
	chmod +x "$dir/zig"
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
"${QSTAR_FAKE_RUST_CC:-cc}" -x c -c "$tmp_c" -o "$out"
rm -f "$tmp_c"
EOF
	chmod +x "$dir/rustc"
}

rm -rf "$tmp"
mkdir -p "$project/src" "$project/include" "$project/tools" \
	"$project/qstar/modules/paths"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if [ -z "$validation_cc" ]; then
	fail "QSTAR_LINUX_VALIDATION_CC must not be empty"
fi
case "$validation_cc" in
*[!A-Za-z0-9_./+-]*)
	fail "QSTAR_LINUX_VALIDATION_CC must be a single compiler command"
	;;
esac
if ! command -v "$validation_cc" >/dev/null 2>&1; then
	fail "compiler '$validation_cc' not found"
fi

cat > "$project/tools/write-generated-header.sh" <<'EOF'
#!/bin/sh
set -eu
out=$1
test -f qstar.lua
test -d src
mkdir -p "$(dirname "$out")"
printf '#define GENERATED_ENV "%s"\n' "${QSTAR_PORTABLE_ENV:-missing}" > "$out"
printf '#define GENERATED_CWD_MARKER "package-root"\n' >> "$out"
EOF
chmod +x "$project/tools/write-generated-header.sh"

cat > "$project/qstar/modules/paths/paths.qsm" <<'EOF'
local M = {}

function M.generated_dir()
  return qstar.join("build", "qstar", "generated")
end

function M.include_dir()
  return qstar.join("include")
end

return M
EOF

cat > "$project/include/port.h" <<'EOF'
#define PORT_VALUE 12
EOF

cat > "$project/src/main.c" <<'EOF'
#include "port.h"
#include "generated.h"

int main(void) {
  return PORT_VALUE > 0 && GENERATED_ENV[0] && GENERATED_CWD_MARKER[0] ? 0 : 1;
}
EOF

cat > "$project/qstar.lua" <<EOF
local paths = qstar.import_module("qstar/modules/paths")
local generated_dir = paths.generated_dir()

qstar.project {
  name = "linux-validation",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = generated_dir,
  compile_commands = "build",
}

qstar.toolset "validation" {
  tools = {
    c = { compiler = qstar.cli {"$validation_cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"$validation_cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"$validation_cc"},
  },
}

qstar.config "validation_tools" {
  toolset = "//:validation",
}

qstar.custom_target "generated_header" {
  outputs = {
    qstar.output(qstar.join(generated_dir, "generated.h")),
  },
  command = qstar.cli {
    "tools/write-generated-header.sh",
    qstar.output(0),
  },
}

qstar.executable "app" {
  configs = {
    "//:validation_tools",
  },
  sources = {
    "src/main.c",
  },
  lang = {
    c = {
      include_dirs = {
        paths.include_dir(),
        generated_dir,
      },
      private_headers = {
        qstar.output(qstar.join(generated_dir, "generated.h")),
      },
    },
  },
}

qstar.group "all" {
  deps = {
    "//:app",
  },
}
EOF

case "$host" in
Linux)
	printf 'qstar-linux-validation: host=linux mode=full\n'
	;;
*)
	printf 'qstar-linux-validation: host=%s mode=limited-linux-path\n' "$host"
	;;
esac
printf 'qstar-linux-validation: depfile_compiler=%s\n' "$validation_cc"

"$qstar" --file "$project/qstar.lua" check //... > "$tmp/check.out" 2> "$tmp/check.err"
contains "$tmp/check.out" "status ok"

QSTAR_PORTABLE_ENV=qstar-linux-validation \
	"$qstar" --file "$project/qstar.lua" build //:all --progress off \
	> "$tmp/build.out" 2> "$tmp/build.err"
contains "$tmp/build.out" "group_target label=//:all"
contains "$tmp/build.out" "status ok"
test -x "$project/build/qstar/out/___app/app" ||
	fail "portable app artifact missing"
"$project/build/qstar/out/___app/app"

test -f "$project/build/qstar/generated/generated.h" ||
	fail "generated header missing under generated_dir"
contains "$project/build/qstar/generated/generated.h" \
	'GENERATED_ENV "qstar-linux-validation"'
contains "$project/build/qstar/generated/generated.h" \
	'GENERATED_CWD_MARKER "package-root"'
test -f "$project/build/qstar/out/___app/obj0.d" ||
	fail "compiler depfile missing"
contains "$project/build/qstar/compile_commands.json" "src/main.c"
contains "$project/build/qstar/compile_commands.json" "build/qstar/generated"
not_contains "$project/build/qstar/compile_commands.json" "\\"

cat > "$project/include/port.h" <<'EOF'
#define PORT_VALUE 13
EOF
"$qstar" --file "$project/qstar.lua" build //:app --explain-cache \
	> "$tmp/rebuild.out" 2> "$tmp/rebuild.err"
contains "$tmp/rebuild.out" "cache_miss id=//:app:compile:0"
contains "$tmp/rebuild.out" "reason=depfile-changed"
contains "$tmp/rebuild.out" "status ok"

${MAKE:-make} install PREFIX="$install_root" > "$tmp/install.out" 2> "$tmp/install.err"
test -x "$install_root/bin/qstar" || fail "installed qstar missing"
"$install_root/bin/qstar" --version > "$tmp/install-version.out" 2> "$tmp/install-version.err"
contains "$tmp/install-version.out" "qstar "
test -f "$install_root/share/doc/qstar/wiki/AI_INDEX.md" ||
	fail "installed AI index missing"
test -f "$install_root/share/man/man1/qstar.1" ||
	fail "installed qstar(1) manpage missing"
test -f "$install_root/share/man/man5/qstar-lua.5" ||
	fail "installed qstar-lua(5) manpage missing"
test -f "$install_root/share/qstar/languages/zig/zig.qsm" ||
	fail "installed Zig language provider manifest missing"
test -f "$install_root/share/qstar/languages/zig/provider.lua" ||
	fail "installed Zig language provider implementation missing"
test -f "$install_root/share/qstar/languages/rust/rust.qsm" ||
	fail "installed Rust language provider manifest missing"
test -f "$install_root/share/qstar/languages/rust/provider.lua" ||
	fail "installed Rust language provider implementation missing"
QSTAR_DOC_DIR="$install_root/share/doc/qstar" \
	"$install_root/bin/qstar" docs --path > "$tmp/install-docs-path.out" \
	2> "$tmp/install-docs-path.err"
contains "$tmp/install-docs-path.out" "$install_root/share/doc/qstar/wiki"

installed_zig="$tmp/installed-zig"
mkdir -p "$installed_zig/src" "$installed_zig/tools"
cat > "$installed_zig/src/main.zig" <<'EOF'
pub export fn installed_value() i32 {
    return 23;
}
EOF
cat > "$installed_zig/tools/fake-zig" <<'EOF'
#!/bin/sh
out=
while [ "$#" -gt 0 ]; do
  case "$1" in
    -femit-bin=*)
      out=${1#-femit-bin=}
      ;;
  esac
  shift
done
test -n "$out" || exit 2
mkdir -p "$(dirname "$out")"
printf 'installed zig object\n' > "$out"
EOF
chmod +x "$installed_zig/tools/fake-zig"
cat > "$installed_zig/qstar.lua" <<'EOF'
local zig = qstar.use_language("zig")

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    zig = zig.tools {
      compiler = qstar.cli {"tools/fake-zig"},
    },
  },
}

qstar.config "zig_release" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      optimize = "ReleaseSafe",
    },
  },
}

qstar.staticlib "core" {
  configs = {
    "//:zig_release",
  },
  sources = {
    zig.object("src/main.zig"),
  },
}
EOF
"$install_root/bin/qstar" --file "$installed_zig/qstar.lua" build //:core \
	> "$tmp/install-zig-provider.out" 2> "$tmp/install-zig-provider.err"
contains "$tmp/install-zig-provider.out" "status ok"
test -f "$installed_zig/build/qstar/out/___core/obj0.o" ||
	fail "installed standard Zig provider did not produce object"

installed_init_zig="$tmp/installed-init-zig"
installed_fake_zig_bin="$tmp/installed-fake-zig-bin"
write_fake_zig_bin "$installed_fake_zig_bin"
"$install_root/bin/qstar" init app "$installed_init_zig" --use-language=zig \
	> "$tmp/install-init-zig.out" 2> "$tmp/install-init-zig.err"
contains "$tmp/install-init-zig.out" "vendor qstar/languages/zig"
contains "$tmp/install-init-zig.out" "scaffold zig app"
test -f "$installed_init_zig/qstar/languages/zig/zig.qsm" ||
	fail "installed qstar init did not vendor Zig manifest"
test -f "$installed_init_zig/qstar/languages/zig/provider.lua" ||
	fail "installed qstar init did not vendor Zig implementation"
QSTAR_FAKE_ZIG_CC="$validation_cc" PATH="$installed_fake_zig_bin:$PATH" \
	"$install_root/bin/qstar" --file "$installed_init_zig/qstar.lua" build //:app \
	> "$tmp/install-init-zig-build.out" 2> "$tmp/install-init-zig-build.err"
contains "$tmp/install-init-zig-build.out" "status ok"
"$installed_init_zig/build/qstar/out/___app/app" ||
	fail "installed qstar init Zig app binary failed"

installed_init_rust="$tmp/installed-init-rust"
installed_fake_rust_bin="$tmp/installed-fake-rust-bin"
write_fake_rust_bin "$installed_fake_rust_bin"
"$install_root/bin/qstar" init app "$installed_init_rust" --use-language=rust \
	> "$tmp/install-init-rust.out" 2> "$tmp/install-init-rust.err"
contains "$tmp/install-init-rust.out" "vendor qstar/languages/rust"
contains "$tmp/install-init-rust.out" "scaffold rust app"
test -f "$installed_init_rust/qstar/languages/rust/rust.qsm" ||
	fail "installed qstar init did not vendor Rust manifest"
test -f "$installed_init_rust/qstar/languages/rust/provider.lua" ||
	fail "installed qstar init did not vendor Rust implementation"
QSTAR_FAKE_RUST_CC="$validation_cc" PATH="$installed_fake_rust_bin:$PATH" \
	"$install_root/bin/qstar" --file "$installed_init_rust/qstar.lua" build //:app \
	> "$tmp/install-init-rust-build.out" 2> "$tmp/install-init-rust-build.err"
contains "$tmp/install-init-rust-build.out" "status ok"
"$installed_init_rust/build/qstar/out/___app/app" ||
	fail "installed qstar init Rust app binary failed"

printf 'qstar-linux-validation: install_prefix=%s\n' "$install_root"
printf 'qstar-linux-validation: passed\n'
