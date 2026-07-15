#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-wiki-cli-sync.$$

fail() {
	printf 'qstar-wiki-cli-sync: %s\n' "$1" >&2
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

forbid_tree() {
	pattern=$1
	shift
	if grep -R -n -E "$pattern" "$@" > "$tmp/forbidden.out"; then
		cat "$tmp/forbidden.out" >&2
		fail "public CLI/wiki surface contains forbidden legacy syntax"
	fi
}

cleanup() {
	rm -rf "$tmp"
}

rm -rf "$tmp"
mkdir -p "$tmp"
trap cleanup EXIT HUP INT TERM

"$qstar" --help > "$tmp/help-root.out" 2> "$tmp/help-root.err"
contains "$tmp/help-root.out" "qstar [options] commands [--format text|json]"
contains "$tmp/help-root.out" "qstar [options] <project-command> [command-options]"
contains "$tmp/help-root.out" "qstar [options] stage <label> [--root path] [--dry-run]"
contains "$tmp/help-root.out" "qstar [options] clean [label]"
contains "$tmp/help-root.out" "qstar init app|lib|tool|empty|workspace"
not_contains "$tmp/help-root.out" "qstar [options] install"
not_contains "$tmp/help-root.out" "usage: qstar install"
not_contains "$tmp/help-root.out" "--profile"
not_contains "$tmp/help-root.out" "--target triple"
not_contains "$tmp/help-root.out" "--toolchain"
not_contains "$tmp/help-root.out" "--stdlib"
not_contains "$tmp/help-root.out" "clean [--target label]"

"$qstar" help install > "$tmp/help-install.out" 2> "$tmp/help-install.err"
contains "$tmp/help-install.out" "usage: qstar"
contains "$tmp/help-install.out" "qstar [options] <project-command> [command-options]"
not_contains "$tmp/help-install.out" "qstar [options] install"
not_contains "$tmp/help-install.out" "Compatibility command for conventional"
not_contains "$tmp/help-install.out" "install requires --prefix"

for help_cmd in build test stage dry-run emit-ninja lint fmt list-targets check \
	last-failure replay docs commands; do
	"$qstar" "$help_cmd" --help > "$tmp/help-$help_cmd.out" \
		2> "$tmp/help-$help_cmd.err"
	contains "$tmp/help-$help_cmd.out" "usage: qstar"
	not_contains "$tmp/help-$help_cmd.out" "--profile"
	not_contains "$tmp/help-$help_cmd.out" "--toolchain"
	not_contains "$tmp/help-$help_cmd.out" "--stdlib"
done
contains "$tmp/help-docs.out" "Configurable build docs cover qstar.option, qstar.variant, qstar.objectlib"
contains "$tmp/help-emit-ninja.out" "activated GLP compile/objectlib actions"

contains man/man1/qstar.1 ".It Ic commands"
contains man/man1/qstar.1 ".It Ar project-command"
contains man/man1/qstar.1 ".It Ic stage"
contains man/man1/qstar.1 ".It Ic clean"
contains man/man1/qstar.1 "single target output when a label is"
contains man/man1/qstar.1 ".It Fl D Ar name=value"
contains man/man1/qstar.1 "activated GLP raw source object actions"
not_contains man/man1/qstar.1 ".It Ic install"
not_contains man/man1/qstar.1 "Fl -target"
not_contains man/man1/qstar.1 "qstar install"
not_contains man/man1/qstar.1 "install requires --prefix"

contains man/man5/qstar-lua.5 ".It Ic qstar.option"
contains man/man5/qstar-lua.5 ".It Ic qstar.variant"
contains man/man5/qstar-lua.5 ".It Ic qstar.objectlib"
contains man/man5/qstar-lua.5 "Explicit provider source tokens already carry"
contains man/man5/qstar-lua.5 "Language-specific options must live under"
contains man/man5/qstar-lua.5 "activated language providers add dynamic namespaces"

"$qstar" docs --show reference/qstar-lua.md > "$tmp/wiki-qstar-lua.out" \
	2> "$tmp/wiki-qstar-lua.err"
test ! -s "$tmp/wiki-qstar-lua.err" ||
	fail "docs --show reference/qstar-lua.md wrote stderr"
contains "$tmp/wiki-qstar-lua.out" "qstar commands --format json"
contains "$tmp/wiki-qstar-lua.out" "Command name과 alias는"
contains "$tmp/wiki-qstar-lua.out" "qstar.objectlib"
contains "$tmp/wiki-qstar-lua.out" "Stella/Ninja/explain/dry-run/action-log/replay/compile_commands"
not_contains "$tmp/wiki-qstar-lua.out" "qstar [options] install"
not_contains "$tmp/wiki-qstar-lua.out" "qstar install //"

"$qstar" docs --show reference/configurable-build-surface.md \
	> "$tmp/wiki-configurable.out" 2> "$tmp/wiki-configurable.err"
test ! -s "$tmp/wiki-configurable.err" ||
	fail "docs --show reference/configurable-build-surface.md wrote stderr"
contains "$tmp/wiki-configurable.out" "현재 지원 문법 reference"
contains "$tmp/wiki-configurable.out" "현재 지원 상태"
contains "$tmp/wiki-configurable.out" "Stella/Ninja/explain/dry-run/action-log/replay/compile_commands"
not_contains "$tmp/wiki-configurable.out" "아직 runtime에 모두"
not_contains "$tmp/wiki-configurable.out" "다음 구현 라운드"

"$qstar" docs --show reference/generic-workflows.md > "$tmp/wiki-workflows.out" \
	2> "$tmp/wiki-workflows.err"
test ! -s "$tmp/wiki-workflows.err" ||
	fail "docs --show reference/generic-workflows.md wrote stderr"
contains "$tmp/wiki-workflows.out" 'built-in `qstar install`'
contains "$tmp/wiki-workflows.out" "qstar.step.export_stage"
contains "$tmp/wiki-workflows.out" "install-local"
not_contains "$tmp/wiki-workflows.out" "qstar install //"

contains docs/configurable-build-surface.md "현재 지원 문법 reference"
contains docs/configurable-build-surface.md "현재 지원 상태"
contains docs/syntax.md "current configurable build surface reference"
contains docs/README.md "current-surface reference"
contains wiki/AI_INDEX.md "Configurable build surface current public reference"
contains src/lsp.c "qstar.objectlib"
contains src/lsp.c "compile_context"
contains src/lsp.c "value"
contains src/lsp.c "choices"

forbid_tree 'qstar\.profile|--profile|--toolchain|--stdlib|QSTAR_PROFILE|QSTAR_TARGET|qstar\.exe[[:space:]]*["({]|qstar\.genrule\b|qstar\.config_header\b|qstar \[options\] install|usage: qstar install|qstar install //|install //:|qstar-install-manifest-v2|Compatibility command for conventional|clean --target|clean \[--target|Fl -target|hidden profile|toolchain/profile' \
	README.md README.ko.md docs wiki man \
	editors/vscode/qstar/snippets editors/vscode/qstar/syntaxes \
	tools/sync-github-wiki.sh

if rg -n -- '--qstar-internal-(target|toolchain|stdlib)' include src >/dev/null; then
	echo "legacy profile-era internal CLI option resurfaced" >&2
	exit 1
fi

mkdir -p "$tmp/project-command-ok/data"
printf 'payload\n' > "$tmp/project-command-ok/data/input.txt"
cat > "$tmp/project-command-ok/qstar.lua" <<'EOF'
qstar.stage "layout" {
  root = "stage/layout",
  files = {
    qstar.stage_file("data/input.txt", "share/input.txt"),
  },
}

qstar.command "install" {
  aliases = {"install-local"},
  steps = {
    qstar.step.export_stage("//:layout", {
      to = "exports/install",
    }),
  },
}
EOF
"$qstar" --file "$tmp/project-command-ok/qstar.lua" commands --format json \
	> "$tmp/project-command-ok.json" 2> "$tmp/project-command-ok.err"
contains "$tmp/project-command-ok.json" "\"name\":\"install\""
contains "$tmp/project-command-ok.json" "install-local"
"$qstar" --file "$tmp/project-command-ok/qstar.lua" install \
	> "$tmp/project-command-ok.out" 2> "$tmp/project-command-install.err"
contains "$tmp/project-command-ok.out" "command install"
contains "$tmp/project-command-ok.out" "command_export_stage label=//:layout"
test -f "$tmp/project-command-ok/exports/install/share/input.txt" ||
	fail "project command install did not export layout"

reserved_commands="help version docs init lsp fmt daemon commands command list-targets query doctor check lint explain dry-run emit-ninja build test stage why-rebuild clean log last-failure action-log replay"

for reserved in $reserved_commands; do
	dir="$tmp/reserved-name-$reserved"
	mkdir -p "$dir"
	cat > "$dir/qstar.lua" <<EOF
qstar.command "$reserved" {
  steps = {
    qstar.step.check("//..."),
  },
}
EOF
	if "$qstar" --file "$dir/qstar.lua" commands > "$dir.out" 2> "$dir.err"; then
		fail "reserved project command '$reserved' unexpectedly succeeded"
	fi
	contains "$dir.err" "reserved by the qstar CLI"

	dir="$tmp/reserved-alias-$reserved"
	mkdir -p "$dir"
	cat > "$dir/qstar.lua" <<EOF
qstar.command "ok" {
  aliases = {"$reserved"},
  steps = {
    qstar.step.check("//..."),
  },
}
EOF
	if "$qstar" --file "$dir/qstar.lua" commands > "$dir.out" 2> "$dir.err"; then
		fail "reserved project command alias '$reserved' unexpectedly succeeded"
	fi
	contains "$dir.err" "reserved by the qstar CLI"
done

printf 'qstar-wiki-cli-sync: passed\n'
