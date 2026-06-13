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

qstar.profile "default" {
  cc = "$validation_cc",
  tool_overrides = {
    "qstar-port-gen=tools/write-generated-header.sh",
  },
}

qstar.custom_target "generated_header" {
  outputs = {
    qstar.output(qstar.join(generated_dir, "generated.h")),
  },
  command = qstar.cli {
    "qstar-port-gen",
    qstar.output(0),
  },
}

qstar.executable "app" {
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
QSTAR_DOC_DIR="$install_root/share/doc/qstar" \
	"$install_root/bin/qstar" docs --path > "$tmp/install-docs-path.out" \
	2> "$tmp/install-docs-path.err"
contains "$tmp/install-docs-path.out" "$install_root/share/doc/qstar/wiki"

printf 'qstar-linux-validation: install_prefix=%s\n' "$install_root"
printf 'qstar-linux-validation: passed\n'
