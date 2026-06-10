#!/bin/sh
set -eu

qstar=${QSTAR_TEST_QSTAR:-build/bin/qstar}
tmp=${TMPDIR:-/tmp}/qstar-smoke.$$

fail() {
	echo "qstar-smoke: $*" >&2
	exit 1
}

contains() {
	file=$1
	pat=$2
	grep -F -q -- "$pat" "$file" || fail "missing pattern '$pat' in $file"
}

send_lsp() {
	payload=$1
	len=$(printf "%s" "$payload" | wc -c | tr -d ' ')
	printf 'Content-Length: %s\r\n\r\n%s' "$len" "$payload"
}

rm -rf "$tmp"
mkdir -p "$tmp/src"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cat > "$tmp/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
}
EOF

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:app --jobs 2 --schedule-trace > "$tmp/first.out" 2> "$tmp/first.err"
contains "$tmp/first.out" "qstar build v2"
contains "$tmp/first.out" "executor-policy version=v3"
contains "$tmp/first.out" "parallel=optional jobs=2"
contains "$tmp/first.out" "action_dag target=//:app"
contains "$tmp/first.out" "schedule_action id=//:app:compile:0"
contains "$tmp/first.out" "status=run"
contains "$tmp/first.out" "status ok"
test -f "$tmp/.qstar/state/actions.json" || fail "missing action state"
test -f "$tmp/.qstar/state/graph.json" || fail "missing graph snapshot"
test -f "$tmp/.qstar/state/last-summary.json" || fail "missing build summary"
contains "$tmp/.qstar/state/graph.json" "\"schema\":\"qstar-graph-snapshot-v1\""
contains "$tmp/.qstar/state/graph.json" "\"label\":\"//:app\""
contains "$tmp/.qstar/state/last-summary.json" "\"schema\":\"qstar-build-summary-v1\""
contains "$tmp/.qstar/state/last-summary.json" "\"status\":\"success\""
contains "$tmp/.qstar/state/actions.json" "\"argv_key\":"
contains "$tmp/.qstar/state/actions.json" "\"env_key\":"
contains "$tmp/.qstar/state/actions.json" "\"input_key\":"
test -f "$tmp/compile_commands.json" || fail "missing compile_commands.json"
contains "$tmp/compile_commands.json" "src/main.c"

"$qstar" --file "$tmp/qstar.lua" lint > "$tmp/lint-ok.out" 2> "$tmp/lint-ok.err"
contains "$tmp/lint-ok.out" "qstar lint v1"
contains "$tmp/lint-ok.out" "summary errors=0 warnings=0"
contains "$tmp/lint-ok.out" "status ok"
"$qstar" --file "$tmp/qstar.lua" lint --format json > "$tmp/lint-json.out" 2> "$tmp/lint-json.err"
contains "$tmp/lint-json.out" "\"schema\":\"qstar-lint-v1\""
contains "$tmp/lint-json.out" "\"diagnostics\":[]"
"$qstar" --file "$tmp/qstar.lua" list-targets --format json > "$tmp/targets-json.out" 2> "$tmp/targets-json.err"
contains "$tmp/targets-json.out" "\"schema\":\"qstar-targets-v1\""
contains "$tmp/targets-json.out" "\"target_count\":1"
contains "$tmp/targets-json.out" "\"generated_action_count\":0"
contains "$tmp/targets-json.out" "\"label\":\"//:app\""
contains "$tmp/targets-json.out" "\"is_test\":false"
contains "$tmp/targets-json.out" "\"installable\":true"

mkdir -p "$tmp/fmt"
cat > "$tmp/fmt/qstar.lua" <<'EOF'
qstar.executable "app" {
sources={"src/main.c"},
deps={"//lib:core"},
}
EOF
if "$qstar" fmt --check "$tmp/fmt/qstar.lua" > "$tmp/fmt-check-before.out" 2> "$tmp/fmt-check-before.err"; then
	fail "qstar fmt --check unexpectedly accepted unformatted file"
fi
contains "$tmp/fmt-check-before.out" "qstar fmt v1"
contains "$tmp/fmt-check-before.out" "status needs-format"
"$qstar" fmt --stdout "$tmp/fmt/qstar.lua" > "$tmp/fmt-stdout.out" 2> "$tmp/fmt-stdout.err"
contains "$tmp/fmt-stdout.out" "qstar.executable \"app\" {"
contains "$tmp/fmt-stdout.out" "  sources = {"
contains "$tmp/fmt-stdout.out" "    \"src/main.c\","
contains "$tmp/fmt-stdout.out" "  deps = {"
contains "$tmp/fmt-stdout.out" "    \"//lib:core\","
"$qstar" fmt "$tmp/fmt/qstar.lua" > "$tmp/fmt-write.out" 2> "$tmp/fmt-write.err"
contains "$tmp/fmt-write.out" "status formatted"
"$qstar" fmt --check "$tmp/fmt/qstar.lua" > "$tmp/fmt-check-after.out" 2> "$tmp/fmt-check-after.err"
contains "$tmp/fmt-check-after.out" "status ok"
"$qstar" --file "$tmp/fmt/qstar.lua" fmt --check > "$tmp/fmt-file-option.out" 2> "$tmp/fmt-file-option.err"
contains "$tmp/fmt-file-option.out" "status ok"

if "$qstar" --file "$tmp/qstar.lua" lint //:missing > "$tmp/lint-missing-label.out" 2> "$tmp/lint-missing-label.err"; then
	fail "unknown lint label unexpectedly succeeded"
fi
contains "$tmp/lint-missing-label.out" "QSTAR010"
contains "$tmp/lint-missing-label.out" "unknown target label"

lsp_uri="file://$tmp/qstar.lua"
{
	send_lsp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_uri"'","languageId":"qstar","version":1,"text":"qstar.executable \"app\" {\n  sources = {\"src/main.c\"},\n}\n"}}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$lsp_uri"'","version":2},"contentChanges":[{"text":"qstar.executable \"app\" {\n  sources = {\"src/main.c\"},\n}\n"}]}}'
	send_lsp '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$lsp_uri"'"},"position":{"line":0,"character":7}}}'
	send_lsp '{"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$lsp_uri"'"},"position":{"line":1,"character":2}}}'
	send_lsp '{"jsonrpc":"2.0","id":4,"method":"shutdown","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"exit","params":{}}'
} | "$qstar" lsp --stdio > "$tmp/lsp.out" 2> "$tmp/lsp.err"
contains "$tmp/lsp.out" "\"name\":\"qstar-lsp\""
contains "$tmp/lsp.out" "textDocument/publishDiagnostics"
contains "$tmp/lsp.out" "\"diagnostics\":[]"
contains "$tmp/lsp.out" "Create an executable target."
contains "$tmp/lsp.out" "\"label\":\"qstar.configure_file\""

mkdir -p "$tmp/lsp-missing"
cat > "$tmp/lsp-missing/qstar.lua" <<'EOF'
qstar.subdir("foo")
EOF
lsp_bad_uri="file://$tmp/lsp-missing/qstar.lua"
{
	send_lsp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_bad_uri"'","languageId":"qstar","version":1,"text":"qstar.subdir(\"foo\")\n"}}}'
	send_lsp '{"jsonrpc":"2.0","id":2,"method":"shutdown","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"exit","params":{}}'
} | "$qstar" lsp --stdio > "$tmp/lsp-missing.out" 2> "$tmp/lsp-missing.err"
contains "$tmp/lsp-missing.out" "QSTAR002"
contains "$tmp/lsp-missing.out" "missing fragment"

mkdir -p "$tmp/lsp-nav/lib" "$tmp/lsp-nav/app"
touch "$tmp/lsp-nav/qstar.workspace"
cat > "$tmp/lsp-nav/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/cfg.h"),
  defines = {"HAVE_CFG"},
}

qstar.custom_target "gen" {
  outputs = {qstar.output("generated/gen.c")},
  command = qstar.cli {"tools/gen.sh", qstar.output(0)},
}

qstar.subdir("lib")
qstar.subdir("app")
EOF
cat > "$tmp/lsp-nav/lib/lib.qs" <<'EOF'
qstar.staticlib "core" {}
EOF
cat > "$tmp/lsp-nav/app/app.qs" <<'EOF'
qstar.executable "app" {
  deps = {"//lib:core"},
}
EOF
lsp_nav_root_uri="file://$tmp/lsp-nav/qstar.lua"
lsp_nav_lib_uri="file://$tmp/lsp-nav/lib/lib.qs"
lsp_nav_app_uri="file://$tmp/lsp-nav/app/app.qs"
{
	send_lsp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_nav_root_uri"'","languageId":"qstar","version":1,"text":"qstar.configure_file \"cfg\" {\n  output = qstar.output(\"generated/cfg.h\"),\n  defines = {\"HAVE_CFG\"},\n}\n\nqstar.custom_target \"gen\" {\n  outputs = {qstar.output(\"generated/gen.c\")},\n  command = qstar.cli {\"tools/gen.sh\", qstar.output(0)},\n}\n\nqstar.subdir(\"lib\")\nqstar.subdir(\"app\")\n"}}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_nav_lib_uri"'","languageId":"qstar","version":1,"text":"qstar.staticlib \"core\" {}\n"}}}'
	send_lsp '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$lsp_nav_app_uri"'","languageId":"qstar","version":1,"text":"qstar.executable \"app\" {\n  deps = {\"//lib:core\"},\n}\n"}}}'
	send_lsp '{"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$lsp_nav_app_uri"'"},"position":{"line":1,"character":14}}}'
	send_lsp '{"jsonrpc":"2.0","id":3,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$lsp_nav_app_uri"'"},"position":{"line":1,"character":14},"context":{"includeDeclaration":false}}}'
	send_lsp '{"jsonrpc":"2.0","id":4,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$lsp_nav_lib_uri"'"}}}'
	send_lsp '{"jsonrpc":"2.0","id":5,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$lsp_nav_root_uri"'"}}}'
	send_lsp '{"jsonrpc":"2.0","id":6,"method":"workspace/symbol","params":{"query":"core"}}'
	send_lsp '{"jsonrpc":"2.0","id":7,"method":"shutdown","params":{}}'
	send_lsp '{"jsonrpc":"2.0","method":"exit","params":{}}'
} | "$qstar" lsp --stdio > "$tmp/lsp-nav.out" 2> "$tmp/lsp-nav.err"
contains "$tmp/lsp-nav.out" "\"definitionProvider\":true"
contains "$tmp/lsp-nav.out" "\"referencesProvider\":true"
contains "$tmp/lsp-nav.out" "\"documentSymbolProvider\":true"
contains "$tmp/lsp-nav.out" "\"workspaceSymbolProvider\":true"
contains "$tmp/lsp-nav.out" "\"uri\":\"$lsp_nav_lib_uri\""
contains "$tmp/lsp-nav.out" "\"uri\":\"$lsp_nav_app_uri\""
contains "$tmp/lsp-nav.out" "\"name\":\"//lib:core\""
contains "$tmp/lsp-nav.out" "\"name\":\"//:cfg\""
contains "$tmp/lsp-nav.out" "\"name\":\"//:gen\""

vscode_ext="editors/vscode/qstar"
test -f "$vscode_ext/package.json" || fail "missing QStar VSCode package.json"
test -f "$vscode_ext/extension.js" || fail "missing QStar VSCode extension.js"
test -f "$vscode_ext/syntaxes/qstar.tmLanguage.json" || fail "missing QStar grammar"
test -f "$vscode_ext/snippets/qstar.json" || fail "missing QStar snippets"
test -f "$vscode_ext/scripts/package-vsix.sh" || fail "missing QStar VSCode package script"
test -f "$vscode_ext/scripts/check-package.js" || fail "missing QStar VSCode package check"
test -f "$vscode_ext/samples/workspace/qstar.lua" || fail "missing QStar VSCode sample root"
test -f "$vscode_ext/samples/workspace/app/app.qs" || fail "missing QStar VSCode app sample"
test -f "$vscode_ext/samples/workspace/lib/lib.qs" || fail "missing QStar VSCode lib sample"
contains "$vscode_ext/package.json" "\"id\": \"qstar\""
contains "$vscode_ext/package.json" "\"qstar.lua\""
contains "$vscode_ext/package.json" "\".qs\""
contains "$vscode_ext/package.json" "\"qstar.workspace\""
contains "$vscode_ext/package.json" "\"package:vsix\""
contains "$vscode_ext/package.json" "\"directory\": \"qstar/editors/vscode/qstar\""
contains "$vscode_ext/.vscodeignore" "*.vsix"
contains "$vscode_ext/.vscodeignore" "dist/"
contains "$vscode_ext/.vscodeignore" "node_modules/"
contains "$vscode_ext/.vscodeignore" "**/.qstar/**"
contains "$vscode_ext/.vscodeignore" "**/compile_commands.json"
contains "$vscode_ext/package.json" "\"qstar.server.path\""
contains "$vscode_ext/package.json" "\"qstar.trace.server\""
contains "$vscode_ext/package.json" "\"qstarGraph\""
contains "$vscode_ext/package.json" "\"qstar.checkWorkspace\""
contains "$vscode_ext/package.json" "\"qstar.refreshGraph\""
contains "$vscode_ext/package.json" "\"qstar.explainTarget\""
contains "$vscode_ext/package.json" "\"qstar.listTargets\""
contains "$vscode_ext/package.json" "\"qstar.dryRunTarget\""
contains "$vscode_ext/package.json" "\"qstar.buildTarget\""
contains "$vscode_ext/package.json" "\"qstar.testTarget\""
contains "$vscode_ext/package.json" "\"qstar.openActionLog\""
contains "$vscode_ext/package.json" "\"qstar.replayAction\""
contains "$vscode_ext/extension.js" "qstar lsp"
contains "$vscode_ext/extension.js" "registerHoverProvider"
contains "$vscode_ext/extension.js" "registerCompletionItemProvider"
contains "$vscode_ext/extension.js" "registerDefinitionProvider"
contains "$vscode_ext/extension.js" "registerReferenceProvider"
contains "$vscode_ext/extension.js" "registerDocumentSymbolProvider"
contains "$vscode_ext/extension.js" "registerWorkspaceSymbolProvider"
contains "$vscode_ext/extension.js" "registerDocumentFormattingEditProvider"
contains "$vscode_ext/extension.js" "fmt\", \"--stdout"
contains "$vscode_ext/extension.js" "qstar lsp --stdio"
contains "$vscode_ext/extension.js" "registerTreeDataProvider"
contains "$vscode_ext/extension.js" "qstar-targets-v1"
contains "$vscode_ext/extension.js" "last-summary.json"
contains "$vscode_ext/syntaxes/qstar.tmLanguage.json" "entity.name.label.qstar"
contains "$vscode_ext/snippets/qstar.json" "\"qexe\""
contains "$vscode_ext/snippets/qstar.json" "\"qstaticlib\""
contains "$vscode_ext/snippets/qstar.json" "\"qcustom\""
if find "$vscode_ext" -type d -name node_modules | grep . >/dev/null 2>&1; then
	fail "node_modules must not be present under QStar VSCode extension"
fi
if find "$vscode_ext" -path "$vscode_ext/dist" -prune -o -type f -name '*.vsix' -print | grep . >/dev/null 2>&1; then
	fail "VSIX artifacts must not be committed under QStar VSCode extension"
fi
if command -v node >/dev/null 2>&1; then
	node --check "$vscode_ext/extension.js"
	node --check "$vscode_ext/scripts/check-package.js"
	node "$vscode_ext/scripts/check-package.js" "$vscode_ext"
	node -e 'const fs=require("fs"); for (const p of process.argv.slice(1)) JSON.parse(fs.readFileSync(p,"utf8"));' \
		"$vscode_ext/package.json" \
		"$vscode_ext/language-configuration.json" \
		"$vscode_ext/syntaxes/qstar.tmLanguage.json" \
		"$vscode_ext/snippets/qstar.json"
fi

"$qstar" --file "$vscode_ext/samples/workspace/qstar.lua" lint > "$tmp/vscode-sample-lint.out" 2> "$tmp/vscode-sample-lint.err"
contains "$tmp/vscode-sample-lint.out" "qstar lint v1"
contains "$tmp/vscode-sample-lint.out" "status ok"
"$qstar" --file "$vscode_ext/samples/workspace/qstar.lua" list-targets --format json > "$tmp/vscode-sample-targets.out" 2> "$tmp/vscode-sample-targets.err"
contains "$tmp/vscode-sample-targets.out" "\"schema\":\"qstar-targets-v1\""
contains "$tmp/vscode-sample-targets.out" "\"label\":\"//app:app\""
contains "$tmp/vscode-sample-targets.out" "\"label\":\"//lib:core\""
"$qstar" --file "$vscode_ext/samples/workspace/qstar.lua" explain //app:app > "$tmp/vscode-sample-explain.out" 2> "$tmp/vscode-sample-explain.err"
contains "$tmp/vscode-sample-explain.out" "target //app:app"
contains "$tmp/vscode-sample-explain.out" "closure-order [//lib:core, //app:app]"
"$qstar" fmt --check "$vscode_ext/samples/workspace/app/app.qs" > "$tmp/vscode-sample-app-fmt.out" 2> "$tmp/vscode-sample-app-fmt.err"
contains "$tmp/vscode-sample-app-fmt.out" "status ok"
"$qstar" fmt --check "$vscode_ext/samples/workspace/lib/lib.qs" > "$tmp/vscode-sample-lib-fmt.out" 2> "$tmp/vscode-sample-lib-fmt.err"
contains "$tmp/vscode-sample-lib-fmt.out" "status ok"

"$qstar" --file "$tmp/qstar.lua" action-log //:app:compile:0 > "$tmp/action-log.out" 2> "$tmp/action-log.err"
contains "$tmp/action-log.out" "qstar action-log v1"
contains "$tmp/action-log.out" "action //:app:compile:0"
contains "$tmp/action-log.out" "qstar-action-log v2"
contains "$tmp/action-log.out" "argv[0]=cc"
"$qstar" --file "$tmp/qstar.lua" replay //:app:compile:0 > "$tmp/action-replay.out" 2> "$tmp/action-replay.err"
contains "$tmp/action-replay.out" "qstar replay v1"
contains "$tmp/action-replay.out" "action //:app:compile:0"
contains "$tmp/action-replay.out" "cc -c src/main.c"

"$qstar" --file "$tmp/qstar.lua" build //:app > "$tmp/second.out" 2> "$tmp/second.err"
contains "$tmp/second.out" "status=skip"

"$qstar" --file "$tmp/qstar.lua" why-rebuild //:app > "$tmp/why.out" 2> "$tmp/why.err"
contains "$tmp/why.out" "qstar why-rebuild v1"
contains "$tmp/why.out" "reason=output-check"
contains "$tmp/why.out" "status=skip"
rm -f "$tmp/.qstar/out/___app/obj0.o"
"$qstar" --file "$tmp/qstar.lua" why-rebuild //:app > "$tmp/why-output.out" 2> "$tmp/why-output.err"
contains "$tmp/why-output.out" "reason=output-missing"

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return 1 - 1; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:app --explain-cache > "$tmp/third.out" 2> "$tmp/third.err"
contains "$tmp/third.out" "cache_miss id=//:app:compile:0"
contains "$tmp/third.out" "reason=input-changed"
contains "$tmp/third.out" "status=run"

"$qstar" --file "$tmp/qstar.lua" log //:app > "$tmp/log.out" 2> "$tmp/log.err"
contains "$tmp/log.out" "qstar log v1"
contains "$tmp/log.out" "log_file .qstar/logs/___app_compile_0.log"

cat > "$tmp/src/main.c" <<'EOF'
int main(void) { return ; }
EOF

if "$qstar" --file "$tmp/qstar.lua" --diagnostics json build //:app > "$tmp/fail.out" 2> "$tmp/fail.err"; then
	fail "invalid C build unexpectedly succeeded"
fi
contains "$tmp/fail.err" "\"schema\":\"qstar-diagnostic-v1\""
contains "$tmp/fail.err" "\"field\":\"action\""
contains "$tmp/.qstar/state/last-summary.json" "\"status\":\"failure\""
test -f "$tmp/.qstar/logs/last-failure.replay" || fail "missing failure replay"

"$qstar" --file "$tmp/qstar.lua" last-failure > "$tmp/replay.out" 2> "$tmp/replay.err"
contains "$tmp/replay.out" "qstar last-failure v1"
contains "$tmp/replay.out" "cc -c src/main.c"

"$qstar" --file "$tmp/qstar.lua" clean --target //:app > "$tmp/clean-target.out" 2> "$tmp/clean-target.err"
contains "$tmp/clean-target.out" "qstar clean v1"
test ! -d "$tmp/.qstar/out/___app" || fail "target clean left target output"

"$qstar" --file "$tmp/qstar.lua" clean > "$tmp/clean.out" 2> "$tmp/clean.err"
contains "$tmp/clean.out" "clean_all .qstar compile_commands.json"
test ! -d "$tmp/.qstar" || fail "clean left .qstar"
test ! -f "$tmp/compile_commands.json" || fail "clean left compile_commands.json"

mkdir -p "$tmp/lint-canonical/foo"
cat > "$tmp/lint-canonical/qstar.lua" <<'EOF'
qstar.subdir("foo")
EOF
cat > "$tmp/lint-canonical/foo/foo.qs" <<'EOF'
qstar.staticlib "core" {}
EOF
"$qstar" --file "$tmp/lint-canonical/qstar.lua" lint //... > "$tmp/lint-canonical.out" 2> "$tmp/lint-canonical.err"
contains "$tmp/lint-canonical.out" "status ok"

mkdir -p "$tmp/lint-deprecated/foo"
cat > "$tmp/lint-deprecated/qstar.lua" <<'EOF'
qstar.subdir("foo")
EOF
cat > "$tmp/lint-deprecated/foo/qstar.qs" <<'EOF'
qstar.staticlib "core" {}
EOF
"$qstar" --file "$tmp/lint-deprecated/qstar.lua" lint --format json > "$tmp/lint-deprecated.out" 2> "$tmp/lint-deprecated.err"
contains "$tmp/lint-deprecated.out" "\"code\":\"QSTAR003\""
contains "$tmp/lint-deprecated.out" "deprecated-fragment-name"
contains "$tmp/lint-deprecated.out" "\"status\":\"warning\""

mkdir -p "$tmp/lint-missing"
cat > "$tmp/lint-missing/qstar.lua" <<'EOF'
qstar.subdir("foo")
EOF
if "$qstar" --file "$tmp/lint-missing/qstar.lua" lint > "$tmp/lint-missing.out" 2> "$tmp/lint-missing.err"; then
	fail "missing subdir fragment unexpectedly succeeded"
fi
contains "$tmp/lint-missing.out" "QSTAR002"
contains "$tmp/lint-missing.out" "missing fragment"

mkdir -p "$tmp/lint-badroot"
cat > "$tmp/lint-badroot/build.lua" <<'EOF'
qstar.executable "app" {}
EOF
if "$qstar" --file "$tmp/lint-badroot/build.lua" lint > "$tmp/lint-badroot.out" 2> "$tmp/lint-badroot.err"; then
	fail "bad root file naming unexpectedly succeeded"
fi
contains "$tmp/lint-badroot.out" "QSTAR001"
contains "$tmp/lint-badroot.out" "root entry must be qstar.lua"

mkdir -p "$tmp/lint-outside"
cat > "$tmp/lint-outside/qstar.lua" <<'EOF'
qstar.executable "bad" {
  sources = {"../escape.c"},
}
EOF
if "$qstar" --file "$tmp/lint-outside/qstar.lua" lint > "$tmp/lint-outside.out" 2> "$tmp/lint-outside.err"; then
	fail "package-escaping source lint unexpectedly succeeded"
fi
contains "$tmp/lint-outside.out" "QSTAR020"
contains "$tmp/lint-outside.out" "must be package-relative"

mkdir -p "$tmp/lint-duplicate"
cat > "$tmp/lint-duplicate/qstar.lua" <<'EOF'
qstar.executable "dup" {}
qstar.staticlib "dup" {}
EOF
if "$qstar" --file "$tmp/lint-duplicate/qstar.lua" lint > "$tmp/lint-duplicate.out" 2> "$tmp/lint-duplicate.err"; then
	fail "duplicate target lint unexpectedly succeeded"
fi
contains "$tmp/lint-duplicate.out" "QSTAR011"
contains "$tmp/lint-duplicate.out" "duplicate target label"

mkdir -p "$tmp/lint-badlabel"
cat > "$tmp/lint-badlabel/qstar.lua" <<'EOF'
qstar.executable "bad label" {}
EOF
if "$qstar" --file "$tmp/lint-badlabel/qstar.lua" lint > "$tmp/lint-badlabel.out" 2> "$tmp/lint-badlabel.err"; then
	fail "bad label lint unexpectedly succeeded"
fi
contains "$tmp/lint-badlabel.out" "QSTAR010"
contains "$tmp/lint-badlabel.out" "invalid target name"

mkdir -p "$tmp/old-api"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.exe "app" {}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-api.out" 2> "$tmp/old-api.err"; then
	fail "removed qstar.exe unexpectedly succeeded"
fi
contains "$tmp/old-api.err" "qstar.exe removed; use qstar.executable"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.genrule "g" {}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-genrule.out" 2> "$tmp/old-genrule.err"; then
	fail "removed qstar.genrule unexpectedly succeeded"
fi
contains "$tmp/old-genrule.err" "qstar.genrule removed; use qstar.custom_target"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.config_header "cfg" {}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-config.out" 2> "$tmp/old-config.err"; then
	fail "removed qstar.config_header unexpectedly succeeded"
fi
contains "$tmp/old-config.err" "qstar.config_header removed; use qstar.configure_file"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.executable "app" {
  include_dirs = {"include"},
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-include.out" 2> "$tmp/old-include.err"; then
	fail "top-level include_dirs unexpectedly succeeded"
fi
contains "$tmp/old-include.err" "top-level include_dirs is not allowed; use lang.c.include_dirs or lang.cxx.include_dirs"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.executable "app" {
  cxx_standard = "c++20",
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-cxx-standard.out" 2> "$tmp/old-cxx-standard.err"; then
	fail "top-level cxx_standard unexpectedly succeeded"
fi
contains "$tmp/old-cxx-standard.err" "top-level cxx_standard is not allowed; use lang.cxx.standard"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.custom_target "g" {
  tool = "tools/gen.sh",
  outputs = {qstar.output("generated/g.c")},
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" check > "$tmp/old-custom-tool.out" 2> "$tmp/old-custom-tool.err"; then
	fail "custom_target tool syntax unexpectedly succeeded"
fi
contains "$tmp/old-custom-tool.err" "command = qstar.cli"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    rust = {
      include_dirs = {"include"},
    },
  },
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" lint --format json > "$tmp/lang-rust.out" 2> "$tmp/lang-rust.err"; then
	fail "unknown lang namespace unexpectedly succeeded"
fi
contains "$tmp/lang-rust.out" "unknown language namespace lang.rust"
cat > "$tmp/old-api/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      unknown_option = true,
    },
  },
}
EOF
if "$qstar" --file "$tmp/old-api/qstar.lua" lint --format json > "$tmp/lang-c-field.out" 2> "$tmp/lang-c-field.err"; then
	fail "unknown lang.c field unexpectedly succeeded"
fi
contains "$tmp/lang-c-field.out" "unknown field lang.c.unknown_option"

mkdir -p "$tmp/lint-header-source/include" "$tmp/lint-header-source/src"
cat > "$tmp/lint-header-source/include/app.h" <<'EOF'
#define APP_VALUE 1
EOF
cat > "$tmp/lint-header-source/src/main.c" <<'EOF'
#include "app.h"
int main(void) { return APP_VALUE - 1; }
EOF
cat > "$tmp/lint-header-source/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c", "include/app.h"},
  lang = {
    c = {
      include_dirs = {"include"},
    },
  },
}
EOF
"$qstar" --file "$tmp/lint-header-source/qstar.lua" lint --format json > "$tmp/lint-header-source.out" 2> "$tmp/lint-header-source.err"
contains "$tmp/lint-header-source.out" "\"code\":\"QSTAR040\""
contains "$tmp/lint-header-source.out" "use public_headers/private_headers"
contains "$tmp/lint-header-source.out" "\"status\":\"warning\""

mkdir -p "$tmp/lint-public-generated"
cat > "$tmp/lint-public-generated/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/public_config.h"),
  defines = {"HAVE_CFG"},
}

qstar.staticlib "core" {
  public_headers = {qstar.output("generated/public_config.h")},
}
EOF
"$qstar" --file "$tmp/lint-public-generated/qstar.lua" lint --format json > "$tmp/lint-public-generated.out" 2> "$tmp/lint-public-generated.err"
contains "$tmp/lint-public-generated.out" "\"code\":\"QSTAR041\""
contains "$tmp/lint-public-generated.out" "outside include/"

mkdir -p "$tmp/lint-private-dep/include" "$tmp/lint-private-dep/src"
cat > "$tmp/lint-private-dep/include/lib.h" <<'EOF'
int lib_value(void);
EOF
cat > "$tmp/lint-private-dep/include/wrapper.h" <<'EOF'
int wrapper_value(void);
EOF
cat > "$tmp/lint-private-dep/src/lib.c" <<'EOF'
int lib_value(void) { return 1; }
EOF
cat > "$tmp/lint-private-dep/src/wrapper.c" <<'EOF'
int wrapper_value(void) { return 2; }
EOF
cat > "$tmp/lint-private-dep/qstar.lua" <<'EOF'
qstar.staticlib "lib" {
  sources = {"src/lib.c"},
  public_headers = {"include/lib.h"},
  lang = {
    c = {
      public_include_dirs = {"include"},
    },
  },
}

qstar.staticlib "wrapper" {
  sources = {"src/wrapper.c"},
  public_headers = {"include/wrapper.h"},
  private_deps = {"//:lib"},
}
EOF
"$qstar" --file "$tmp/lint-private-dep/qstar.lua" lint > "$tmp/lint-private-dep.out" 2> "$tmp/lint-private-dep.err"
contains "$tmp/lint-private-dep.out" "QSTAR042"
contains "$tmp/lint-private-dep.out" "private dependency '//:lib'"

mkdir -p "$tmp/lint-duplicate-source/src"
cat > "$tmp/lint-duplicate-source/src/shared.c" <<'EOF'
int shared(void) { return 0; }
EOF
cat > "$tmp/lint-duplicate-source/qstar.lua" <<'EOF'
qstar.staticlib "one" {
  sources = {"src/shared.c"},
}

qstar.staticlib "two" {
  sources = {"src/shared.c"},
}
EOF
"$qstar" --file "$tmp/lint-duplicate-source/qstar.lua" lint > "$tmp/lint-duplicate-source.out" 2> "$tmp/lint-duplicate-source.err"
contains "$tmp/lint-duplicate-source.out" "QSTAR043"
contains "$tmp/lint-duplicate-source.out" "used by both '//:one' and '//:two'"

mkdir -p "$tmp/lint-cxx-info/src"
cat > "$tmp/lint-cxx-info/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF
cat > "$tmp/lint-cxx-info/qstar.lua" <<'EOF'
qstar.executable "cpp" {
  sources = {"src/main.cpp"},
}
EOF
"$qstar" --file "$tmp/lint-cxx-info/qstar.lua" lint --format json > "$tmp/lint-cxx-info.out" 2> "$tmp/lint-cxx-info.err"
contains "$tmp/lint-cxx-info.out" "\"code\":\"QSTAR044\""
contains "$tmp/lint-cxx-info.out" "\"severity\":\"info\""
contains "$tmp/lint-cxx-info.out" "\"status\":\"ok\""

mkdir -p "$tmp/lint-cale-toolchain/src"
cat > "$tmp/lint-cale-toolchain/src/plugin.cale" <<'EOF'
fn plugin() -> int { return 0; }
EOF
cat > "$tmp/lint-cale-toolchain/qstar.lua" <<'EOF'
qstar.staticlib "plugin" {
  sources = {"src/plugin.cale"},
}
EOF
"$qstar" --file "$tmp/lint-cale-toolchain/qstar.lua" lint > "$tmp/lint-cale-toolchain.out" 2> "$tmp/lint-cale-toolchain.err"
contains "$tmp/lint-cale-toolchain.out" "QSTAR045"
contains "$tmp/lint-cale-toolchain.out" "toolchain=\"cale\""

mkdir -p "$tmp/lint-visibility"
cat > "$tmp/lint-visibility/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  visibility = {"//app:*"},
}
EOF
if "$qstar" --file "$tmp/lint-visibility/qstar.lua" lint > "$tmp/lint-visibility.out" 2> "$tmp/lint-visibility.err"; then
	fail "invalid visibility lint unexpectedly succeeded"
fi
contains "$tmp/lint-visibility.out" "QSTAR050"
contains "$tmp/lint-visibility.out" "invalid visibility pattern"

mkdir -p "$tmp/lint-output-collision"
cat > "$tmp/lint-output-collision/qstar.lua" <<'EOF'
qstar.custom_target "one" {
  outputs = {qstar.output("generated/same.c")},
  command = qstar.cli {"tools/gen.sh", qstar.output(0)},
}

qstar.custom_target "two" {
  outputs = {qstar.output("generated/same.c")},
  command = qstar.cli {"tools/gen.sh", qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/lint-output-collision/qstar.lua" lint --format json > "$tmp/lint-output-collision.out" 2> "$tmp/lint-output-collision.err"; then
	fail "generated collision lint unexpectedly succeeded"
fi
contains "$tmp/lint-output-collision.out" "\"code\":\"QSTAR060\""
contains "$tmp/lint-output-collision.out" "multiple producers"

mkdir -p "$tmp/lint-orphan/foo"
cat > "$tmp/lint-orphan/qstar.lua" <<'EOF'
qstar.executable "app" {}
EOF
cat > "$tmp/lint-orphan/foo/foo.qs" <<'EOF'
qstar.staticlib "core" {}
EOF
"$qstar" --file "$tmp/lint-orphan/qstar.lua" lint --format json > "$tmp/lint-orphan.out" 2> "$tmp/lint-orphan.err"
contains "$tmp/lint-orphan.out" "\"code\":\"QSTAR071\""
contains "$tmp/lint-orphan.out" "not reached by qstar.subdir()"

mkdir -p "$tmp/tools"
cat > "$tmp/tools/gen-value.sh" <<'EOF'
#!/bin/sh
set -eu
out=$1
mkdir -p "$(dirname "$out")"
cat > "$out" <<'SRC'
int generated_value(void) { return 41; }
SRC
EOF
chmod +x "$tmp/tools/gen-value.sh"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=41", "APP_FEATURE"},
}

qstar.custom_target "make_value" {
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}

qstar.executable "genapp" {
  sources = {"src/main.c", qstar.output("generated/value.c")},
  private_headers = {qstar.output("generated/config.h")},
  lang = {
    c = {
      include_dirs = {"generated"},
    },
  },
}

qstar.run_target "smoke" {
  deps = {"//:genapp"},
  command = qstar.cli {qstar.target_file("//:genapp")},
  timeout = 5,
}

qstar.run_target "marker" {
  command = qstar.cli {"printf", "QSTAR-MARKER\n"},
  timeout = 5,
  marker = "QSTAR-MARKER",
}
EOF

cat > "$tmp/src/main.c" <<'EOF'
#include "config.h"
int generated_value(void);
int main(void) { return generated_value() - APP_VALUE; }
EOF

"$qstar" --file "$tmp/qstar.lua" build //:genapp > "$tmp/generated-first.out" 2> "$tmp/generated-first.err"
contains "$tmp/generated-first.out" "build_action id=//:cfg:generate:0 status=run"
contains "$tmp/generated-first.out" "build_action id=//:make_value:generate:0 status=run"
contains "$tmp/generated-first.out" "status ok"
test -f "$tmp/generated/config.h" || fail "missing generated config header"
test -f "$tmp/generated/value.c" || fail "missing generated source"
contains "$tmp/generated/config.h" "#define APP_VALUE 41"
"$qstar" --file "$tmp/qstar.lua" build //:smoke > "$tmp/run-target-smoke.out" 2> "$tmp/run-target-smoke.err"
contains "$tmp/run-target-smoke.out" "run_target label=//:smoke command=argv"
contains "$tmp/run-target-smoke.out" "status ok"
"$qstar" --file "$tmp/qstar.lua" build //:marker > "$tmp/run-target-marker.out" 2> "$tmp/run-target-marker.err"
contains "$tmp/run-target-marker.out" "run_marker label=//:marker status=matched"
contains "$tmp/run-target-marker.out" "status ok"

mkdir -p "$tmp/qemu/tools"
cat > "$tmp/qemu/tools/fake-qemu.sh" <<'EOF'
#!/bin/sh
set -eu
mode=$1
serial=$2
case "$mode" in
	ok)
		printf "booting\n"
		printf "SERIAL-READY\n" > "$serial"
		;;
	missing)
		printf "booting without marker\n"
		printf "NO-MARKER\n" > "$serial"
		;;
	exit)
		printf "fatal boot error\n" >&2
		exit 7
		;;
	timeout)
		sleep 5
		;;
esac
EOF
chmod +x "$tmp/qemu/tools/fake-qemu.sh"
cat > "$tmp/qemu/qstar.lua" <<'EOF'
qstar.run_target "qemu_ok" {
  command = qstar.cli {"tools/fake-qemu.sh", "ok", "serial.log"},
  timeout = 2,
  marker = "SERIAL-READY",
  marker_log = "serial.log",
}

qstar.run_target "qemu_marker_missing" {
  command = qstar.cli {"tools/fake-qemu.sh", "missing", "serial.log"},
  timeout = 2,
  marker = "SERIAL-READY",
  marker_log = "serial.log",
}

qstar.run_target "qemu_exit" {
  command = qstar.cli {"tools/fake-qemu.sh", "exit", "serial.log"},
  timeout = 2,
  marker = "SERIAL-READY",
  marker_log = "serial.log",
}

qstar.run_target "qemu_timeout" {
  command = qstar.cli {"tools/fake-qemu.sh", "timeout", "serial.log"},
  timeout = 1,
  marker = "SERIAL-READY",
  marker_log = "serial.log",
}
EOF
"$qstar" --file "$tmp/qemu/qstar.lua" build //:qemu_ok > "$tmp/qemu-ok.out" 2> "$tmp/qemu-ok.err"
contains "$tmp/qemu-ok.out" "run_target label=//:qemu_ok command=argv timeout_sec=2 marker=SERIAL-READY marker_log=serial.log"
contains "$tmp/qemu-ok.out" "run_marker label=//:qemu_ok status=matched marker=SERIAL-READY source=marker_log path=serial.log"
contains "$tmp/qemu-ok.out" "status ok"
if "$qstar" --file "$tmp/qemu/qstar.lua" build //:qemu_marker_missing > "$tmp/qemu-missing.out" 2> "$tmp/qemu-missing.err"; then
	fail "qemu marker missing unexpectedly succeeded"
fi
contains "$tmp/qemu-missing.out" "run_target_result label=//:qemu_marker_missing status=marker-missing marker=SERIAL-READY"
contains "$tmp/qemu-missing.out" "marker_log=serial.log"
contains "$tmp/qemu-missing.err" "marker 'SERIAL-READY' was not found"
contains "$tmp/qemu/.qstar/logs/last-failure.replay" "failure_kind=marker-missing"
contains "$tmp/qemu/.qstar/logs/last-failure.replay" "marker_log=serial.log"
"$qstar" --file "$tmp/qemu/qstar.lua" last-failure > "$tmp/qemu-last-failure.out" 2> "$tmp/qemu-last-failure.err"
contains "$tmp/qemu-last-failure.out" "qstar last-failure v1"
contains "$tmp/qemu-last-failure.out" "failure_kind=marker-missing"
contains "$tmp/qemu-last-failure.out" "tools/fake-qemu.sh missing serial.log"
"$qstar" --file "$tmp/qemu/qstar.lua" replay //:qemu_marker_missing:run:0 > "$tmp/qemu-replay.out" 2> "$tmp/qemu-replay.err"
contains "$tmp/qemu-replay.out" "qstar replay v1"
contains "$tmp/qemu-replay.out" "tools/fake-qemu.sh missing serial.log"
if "$qstar" --file "$tmp/qemu/qstar.lua" build //:qemu_exit > "$tmp/qemu-exit.out" 2> "$tmp/qemu-exit.err"; then
	fail "qemu exit failure unexpectedly succeeded"
fi
contains "$tmp/qemu-exit.out" "run_target_result label=//:qemu_exit status=exit-code exit=7"
contains "$tmp/qemu-exit.err" "failed with exit code 7"
contains "$tmp/qemu/.qstar/logs/last-failure.replay" "failure_kind=exit-code"
if "$qstar" --file "$tmp/qemu/qstar.lua" build //:qemu_timeout > "$tmp/qemu-timeout.out" 2> "$tmp/qemu-timeout.err"; then
	fail "qemu timeout unexpectedly succeeded"
fi
contains "$tmp/qemu-timeout.out" "run_target_result label=//:qemu_timeout status=timeout timeout_sec=1"
contains "$tmp/qemu-timeout.err" "timed out after 1 seconds"
contains "$tmp/qemu/.qstar/logs/last-failure.replay" "failure_kind=timeout"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42", "APP_FEATURE"},
}

qstar.custom_target "make_value" {
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}

qstar.executable "genapp" {
  sources = {"src/main.c", qstar.output("generated/value.c")},
  private_headers = {qstar.output("generated/config.h")},
  lang = {
    c = {
      include_dirs = {"generated"},
    },
  },
}
EOF

"$qstar" --file "$tmp/qstar.lua" build //:genapp --explain-cache > "$tmp/generated-second.out" 2> "$tmp/generated-second.err"
contains "$tmp/generated-second.out" "cache_miss id=//:cfg:generate:0"
contains "$tmp/generated-second.out" "cache_miss id=//:genapp:compile:0"
contains "$tmp/generated/config.h" "#define APP_VALUE 42"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.custom_target "one" {
  outputs = {qstar.output("generated/collision.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}

qstar.custom_target "two" {
  outputs = {qstar.output("generated/collision.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" check > "$tmp/collision.out" 2> "$tmp/collision.err"; then
	fail "duplicate generated output unexpectedly succeeded"
fi
contains "$tmp/collision.err" "multiple producers"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.custom_target "bad_out" {
  outputs = {qstar.output("../bad.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" check > "$tmp/outside.out" 2> "$tmp/outside.err"; then
	fail "outside generated output unexpectedly succeeded"
fi
contains "$tmp/outside.err" "must be package-relative"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.custom_target "bad_arg" {
  outputs = {qstar.output("generated/safe.c")},
  command = qstar.cli {"tools/gen-value.sh", "../escape.c"},
}

qstar.executable "bad_gen" {
  sources = {qstar.output("generated/safe.c")},
}
EOF

if "$qstar" --file "$tmp/qstar.lua" build //:bad_gen > "$tmp/bad-arg.out" 2> "$tmp/bad-arg.err"; then
	fail "generated action outside arg unexpectedly succeeded"
fi
contains "$tmp/bad-arg.err" "escapes package root"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.executable "bad_suffix" {
  sources = {"src/main.txt"},
}
EOF
cat > "$tmp/src/main.txt" <<'EOF'
not source
EOF

if "$qstar" --file "$tmp/qstar.lua" check //:bad_suffix > "$tmp/suffix.out" 2> "$tmp/suffix.err"; then
	fail "unsupported suffix unexpectedly succeeded"
fi
contains "$tmp/suffix.err" "unsupported source extension"

cat > "$tmp/tools/cale" <<'EOF'
#!/bin/sh
set -eu
mode=link
out=
src=
prev=
for arg in "$@"; do
  if [ "$prev" = "-o" ]; then
    out=$arg
    prev=
    continue
  fi
  case "$arg" in
    -c) mode=compile ;;
    -o) prev="-o" ;;
    --target=*) ;;
    -*) ;;
    *) src=$arg ;;
  esac
done
if [ "$mode" = "compile" ]; then
  case "$src" in
    *.cale)
      tmp=${TMPDIR:-/tmp}/qstar-fake-cale.$$.c
      printf '%s\n' 'int cale_unit(void) { return 7; }' > "$tmp"
      cc -c "$tmp" -o "$out"
      rm -f "$tmp"
      ;;
    *)
      cc -c "$src" -o "$out"
      ;;
  esac
else
  cc "$@"
fi
EOF
chmod +x "$tmp/tools/cale"

cat > "$tmp/qstar.lua" <<'EOF'
qstar.executable "mixed" {
  toolchain = "cale",
  sources = {"src/main.c", "src/unit.cale"},
}

qstar.staticlib "calelib" {
  toolchain = "cale",
  sources = {"src/unit.cale"},
}
EOF
cat > "$tmp/src/main.c" <<'EOF'
int cale_unit(void);
int main(void) { return cale_unit() - 7; }
EOF
cat > "$tmp/src/unit.cale" <<'EOF'
fn cale_unit() -> int { return 7; }
EOF

PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" dry-run //:mixed > "$tmp/mixed-dry.out" 2> "$tmp/mixed-dry.err"
contains "$tmp/mixed-dry.out" "argv=[cale, -c, src/main.c"
contains "$tmp/mixed-dry.out" "argv=[cale, -c, src/unit.cale"
PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" build //:mixed > "$tmp/mixed-build.out" 2> "$tmp/mixed-build.err"
contains "$tmp/mixed-build.out" "status ok"
PATH="$tmp/tools:$PATH" "$qstar" --file "$tmp/qstar.lua" build //:calelib > "$tmp/cale-only.out" 2> "$tmp/cale-only.err"
contains "$tmp/cale-only.out" "status ok"
contains "$tmp/compile_commands.json" "src/unit.cale"

if PATH=/nonexistent "$qstar" --file "$tmp/qstar.lua" build //:mixed > "$tmp/no-cale.out" 2> "$tmp/no-cale.err"; then
	fail "missing cale compiler unexpectedly succeeded"
fi
contains "$tmp/no-cale.err" "Cale compiler 'cale' not found"

mkdir -p "$tmp/include" "$tmp/src/core_private" "$tmp/lib"
cat > "$tmp/include/core.h" <<'EOF'
int core_value(void);
EOF
cat > "$tmp/src/core_private/core_private.h" <<'EOF'
#define CORE_PRIVATE_VALUE 13
EOF
cat > "$tmp/src/core.c" <<'EOF'
#include "core_private.h"
int core_value(void) { return CORE_PRIVATE_VALUE; }
EOF
cat > "$tmp/src/util.c" <<'EOF'
#include "core.h"
int util_value(void) { return core_value(); }
EOF
cat > "$tmp/src/link_main.c" <<'EOF'
#include "core.h"
int util_value(void);
int main(void) { return util_value() - core_value(); }
EOF
cat > "$tmp/src/bad_private.c" <<'EOF'
#include "core_private.h"
int main(void) { return CORE_PRIVATE_VALUE; }
EOF
cat > "$tmp/qstar.lua" <<'EOF'
qstar.staticlib "core" {
  sources = {"src/core.c"},
  public_headers = {"include/core.h"},
  lang = {
    c = {
      public_include_dirs = {"include"},
      private_include_dirs = {"src/core_private"},
    },
  },
}

qstar.staticlib "util" {
  sources = {"src/util.c"},
  deps = {"//:core"},
}

qstar.executable "linkapp" {
  sources = {"src/link_main.c"},
  deps = {"//:util"},
}

qstar.executable "bad_private" {
  sources = {"src/bad_private.c"},
  deps = {"//:core"},
}

qstar.executable "sysflags" {
  sources = {"src/link_main.c"},
  libs = {"m"},
  lib_dirs = {"lib"},
}

qstar.sharedlib "plugin" {
  sources = {"src/core.c"},
}
EOF

"$qstar" --file "$tmp/qstar.lua" build //:linkapp > "$tmp/linkapp.out" 2> "$tmp/linkapp.err"
contains "$tmp/linkapp.out" "status ok"
case "$(cat "$tmp/.qstar/logs/___linkapp_link_0.log")" in
  *libutil.a*libcore.a*) ;;
  *) fail "link order did not include util before core" ;;
esac

if "$qstar" --file "$tmp/qstar.lua" build //:bad_private > "$tmp/bad-private.out" 2> "$tmp/bad-private.err"; then
	fail "private include propagation unexpectedly succeeded"
fi
contains "$tmp/bad-private.err" "action '//:bad_private:compile:0' failed"

"$qstar" --file "$tmp/qstar.lua" dry-run //:sysflags > "$tmp/sysflags.out" 2> "$tmp/sysflags.err"
contains "$tmp/sysflags.out" "-Llib"
contains "$tmp/sysflags.out" "-lm"

if "$qstar" --file "$tmp/qstar.lua" build //:plugin > "$tmp/shared.out" 2> "$tmp/shared.err"; then
	fail "sharedlib local executor unexpectedly succeeded"
fi
contains "$tmp/shared.err" "sharedlib targets are plan-only"

cat > "$tmp/src/test_pass.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/src/test_fail.c" <<'EOF'
int main(void) { return 3; }
EOF
cat > "$tmp/src/test_timeout.c" <<'EOF'
int main(void) { for (;;) {} return 0; }
EOF
cat > "$tmp/src/install_main.c" <<'EOF'
#include "core.h"
int main(void) { return core_value() - 13; }
EOF
cat > "$tmp/qstar.lua" <<'EOF'
qstar.configure_file "install_cfg" {
  output = qstar.output("generated/install_config.h"),
  defines = {"INSTALL_FEATURE=1"},
}

qstar.test "unit_pass" {
  sources = {"src/test_pass.c"},
}

qstar.test "unit_fail" {
  sources = {"src/test_fail.c"},
}

qstar.test "unit_timeout" {
  sources = {"src/test_timeout.c"},
}

qstar.staticlib "install_core" {
  sources = {"src/core.c"},
  public_headers = {"include/core.h", qstar.output("generated/install_config.h")},
  lang = {
    c = {
      public_include_dirs = {"include"},
      private_include_dirs = {"src/core_private"},
    },
  },
}

qstar.executable "install_app" {
  sources = {"src/install_main.c"},
  deps = {"//:install_core"},
}
EOF

"$qstar" --file "$tmp/qstar.lua" test //:unit_pass > "$tmp/test-pass.out" 2> "$tmp/test-pass.err"
contains "$tmp/test-pass.out" "test_result label=//:unit_pass status=pass"
test -f "$tmp/.qstar/logs/___unit_pass.test.stdout" || fail "missing test stdout log"

if "$qstar" --file "$tmp/qstar.lua" test //:unit_fail > "$tmp/test-fail.out" 2> "$tmp/test-fail.err"; then
	fail "failing test unexpectedly succeeded"
fi
contains "$tmp/test-fail.out" "test_result label=//:unit_fail status=fail exit=3"

if "$qstar" --file "$tmp/qstar.lua" test //:unit_timeout > "$tmp/test-timeout.out" 2> "$tmp/test-timeout.err"; then
	fail "timeout test unexpectedly succeeded"
fi
contains "$tmp/test-timeout.out" "test_result label=//:unit_timeout status=timeout"

"$qstar" --file "$tmp/qstar.lua" build //:install_app > "$tmp/install-build.out" 2> "$tmp/install-build.err"
contains "$tmp/install-build.out" "status ok"
"$qstar" --file "$tmp/qstar.lua" install //:install_app --prefix "$tmp/prefix" --dry-run > "$tmp/install-dry.out" 2> "$tmp/install-dry.err"
contains "$tmp/install-dry.out" "mode dry-run"
contains "$tmp/install-dry.out" "install_file src=.qstar/out/___install_app/install_app"
contains "$tmp/install-dry.out" "install_diff dst=$tmp/prefix/bin/install_app action=would-create"
"$qstar" --file "$tmp/qstar.lua" install //:install_core --prefix "$tmp/prefix" > "$tmp/install-lib.out" 2> "$tmp/install-lib.err"
contains "$tmp/install-lib.out" "status ok"
test -f "$tmp/prefix/lib/libinstall_core.a" || fail "missing installed staticlib"
test -f "$tmp/prefix/include/core.h" || fail "missing installed public header"
test -f "$tmp/prefix/include/generated/install_config.h" || fail "missing installed generated public header"
test -f "$tmp/.qstar/install/manifest.json" || fail "missing install manifest"
contains "$tmp/.qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/.qstar/install/manifest.json" "\"role\":\"staticlib\""
contains "$tmp/.qstar/install/manifest.json" "\"role\":\"header\""
contains "$tmp/.qstar/install/manifest.json" "\"cmake_config\":\"deferred\""

if "$qstar" --file "$tmp/qstar.lua" install //:unit_pass --prefix "$tmp/prefix" > "$tmp/install-test.out" 2> "$tmp/install-test.err"; then
	fail "non-installable test target unexpectedly installed"
fi
contains "$tmp/install-test.err" "not installable"

manual_root=$(pwd)/tests/manual
project_root=$(pwd)/tests/projects

cp -R "$manual_root/c-only" "$tmp/c-only"
"$qstar" --file "$tmp/c-only/qstar.lua" build //:app > "$tmp/c-only-build.out" 2> "$tmp/c-only-build.err"
contains "$tmp/c-only-build.out" "status ok"
"$tmp/c-only/.qstar/out/___app/app"
contains "$tmp/c-only/compile_commands.json" "src/main.c"
contains "$tmp/c-only/compile_commands.json" "src/core.c"
"$qstar" --file "$tmp/c-only/qstar.lua" test //:unit > "$tmp/c-only-test.out" 2> "$tmp/c-only-test.err"
contains "$tmp/c-only-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/c-only/qstar.lua" install //:core --prefix "$tmp/c-only-prefix" > "$tmp/c-only-install.out" 2> "$tmp/c-only-install.err"
contains "$tmp/c-only-install.out" "status ok"
test -f "$tmp/c-only-prefix/lib/libcore.a" || fail "c-only sample did not install libcore.a"
test -f "$tmp/c-only-prefix/include/corpus.h" || fail "c-only sample did not install public header"
contains "$tmp/c-only/compile_commands.json" "tests/unit.c"
"$qstar" --file "$tmp/c-only/qstar.lua" clean > "$tmp/c-only-clean.out" 2> "$tmp/c-only-clean.err"
contains "$tmp/c-only-clean.out" "clean_all .qstar compile_commands.json"
"$qstar" --file "$tmp/c-only/qstar.lua" build //:app > "$tmp/c-only-rebuild.out" 2> "$tmp/c-only-rebuild.err"
contains "$tmp/c-only-rebuild.out" "status=run"
contains "$tmp/c-only-rebuild.out" "status ok"

cp -R "$manual_root/generated" "$tmp/generated-sample"
rm -rf "$tmp/generated-sample/.qstar" "$tmp/generated-sample/generated" "$tmp/generated-sample/compile_commands.json"
"$qstar" --file "$tmp/generated-sample/qstar.lua" build //:app > "$tmp/generated-sample-build.out" 2> "$tmp/generated-sample-build.err"
contains "$tmp/generated-sample-build.out" "status ok"
test -f "$tmp/generated-sample/generated/config.h" || fail "generated sample missing config header"
test -f "$tmp/generated-sample/generated/value.c" || fail "generated sample missing generated source"
"$tmp/generated-sample/.qstar/out/___app/app"
contains "$tmp/generated-sample/compile_commands.json" "src/main.c"
contains "$tmp/generated-sample/compile_commands.json" "generated/value.c"
"$qstar" --file "$tmp/generated-sample/qstar.lua" list-targets --format json > "$tmp/generated-sample-targets.out" 2> "$tmp/generated-sample-targets.err"
contains "$tmp/generated-sample-targets.out" "\"schema\":\"qstar-targets-v1\""
contains "$tmp/generated-sample-targets.out" "\"generated_actions\":["
contains "$tmp/generated-sample-targets.out" "\"config_header\":true"
contains "$tmp/generated-sample-targets.out" "\"label\":\"//:generated_value\""
"$qstar" --file "$tmp/generated-sample/qstar.lua" clean > "$tmp/generated-sample-clean.out" 2> "$tmp/generated-sample-clean.err"
"$qstar" --file "$tmp/generated-sample/qstar.lua" build //:app > "$tmp/generated-sample-rebuild.out" 2> "$tmp/generated-sample-rebuild.err"
contains "$tmp/generated-sample-rebuild.out" "status ok"

cp -R "$manual_root/mixed-cale" "$tmp/mixed-sample"
"$qstar" --file "$tmp/mixed-sample/qstar.lua" dry-run //:mixed > "$tmp/mixed-sample-dry.out" 2> "$tmp/mixed-sample-dry.err"
contains "$tmp/mixed-sample-dry.out" "argv=[cale, -c, src/main.c"
contains "$tmp/mixed-sample-dry.out" "argv=[cale, -c, src/plugin.cale"
contains "$tmp/mixed-sample-dry.out" "rule provider=native final_action=link output_group=exe"

cp -R "$project_root/c-app-lib-test" "$tmp/project-c"
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app > "$tmp/project-c-build.out" 2> "$tmp/project-c-build.err"
contains "$tmp/project-c-build.out" "status ok"
"$tmp/project-c/.qstar/out/___app/app"
"$qstar" --file "$tmp/project-c/qstar.lua" test //:unit > "$tmp/project-c-test.out" 2> "$tmp/project-c-test.err"
contains "$tmp/project-c-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/project-c/qstar.lua" install //:core --prefix "$tmp/project-c-prefix" > "$tmp/project-c-install.out" 2> "$tmp/project-c-install.err"
test -f "$tmp/project-c-prefix/lib/libcore.a" || fail "project corpus c lib did not install"
test -f "$tmp/project-c-prefix/include/corpus.h" || fail "project corpus c header did not install"
contains "$tmp/project-c/.qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/project-c/.qstar/install/manifest.json" "\"role\":\"staticlib\""
contains "$tmp/project-c/.qstar/install/manifest.json" "\"cmake_config\":\"deferred\""
contains "$tmp/project-c/compile_commands.json" "src/core.c"
contains "$tmp/project-c/compile_commands.json" "tests/unit.c"
contains "$tmp/project-c/.qstar/state/graph.json" "\"schema\":\"qstar-graph-snapshot-v1\""
contains "$tmp/project-c/.qstar/state/graph.json" "\"label\":\"//:app\""
contains "$tmp/project-c/.qstar/state/last-summary.json" "\"schema\":\"qstar-build-summary-v1\""
contains "$tmp/project-c/.qstar/state/last-summary.json" "\"status\":\"success\""
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app > "$tmp/project-c-skip.out" 2> "$tmp/project-c-skip.err"
contains "$tmp/project-c-skip.out" "status=skip"
cat > "$tmp/project-c/src/main.c" <<'EOF'
#include "corpus.h"
int main(void) { return corpus_value() - 31; }
EOF
"$qstar" --file "$tmp/project-c/qstar.lua" build //:app --explain-cache > "$tmp/project-c-rebuild.out" 2> "$tmp/project-c-rebuild.err"
contains "$tmp/project-c-rebuild.out" "cache_miss id=//:app:compile:0"
contains "$tmp/project-c-rebuild.out" "status ok"

if command -v c++ >/dev/null 2>&1; then
	cp -R "$project_root/cxx-mixed" "$tmp/project-cxx"
	"$qstar" --file "$tmp/project-cxx/qstar.lua" build //:mixed --jobs 2 --schedule-trace > "$tmp/project-cxx-build.out" 2> "$tmp/project-cxx-build.err"
	contains "$tmp/project-cxx-build.out" "parallel_compile target=//:mixed jobs=2 sources=2 mode=process-v2"
	contains "$tmp/project-cxx-build.out" "parallel_batch target=//:mixed jobs=2 total=2 policy=fifo"
	contains "$tmp/project-cxx-build.out" "schedule_action id=//:mixed:compile:0"
	contains "$tmp/project-cxx-build.out" "schedule_action id=//:mixed:compile:1"
	contains "$tmp/project-cxx-build.out" "parallel_event target=//:mixed event=start id=//:mixed:compile:0"
	contains "$tmp/project-cxx-build.out" "parallel_event target=//:mixed event=finish"
	contains "$tmp/project-cxx-build.out" "status ok"
	"$tmp/project-cxx/.qstar/out/___mixed/mixed"
	contains "$tmp/project-cxx/compile_commands.json" "src/cpp.cpp"
	contains "$tmp/project-cxx/compile_commands.json" "src/main.c"
fi

mkdir -p "$tmp/fanout/src"
cat > "$tmp/fanout/src/a.c" <<'EOF'
#include "config.h"
int a_value(void) { return FANOUT_VALUE; }
EOF
cat > "$tmp/fanout/src/b.c" <<'EOF'
#include "config.h"
int b_value(void) { return FANOUT_VALUE + 1; }
EOF
cat > "$tmp/fanout/src/main.c" <<'EOF'
int a_value(void);
int b_value(void);
int main(void) { return a_value() + b_value() - 15; }
EOF
cat > "$tmp/fanout/qstar.lua" <<'EOF'
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"FANOUT_VALUE=7"},
}

qstar.executable "app" {
  sources = {"src/a.c", "src/b.c", "src/main.c"},
  private_headers = {qstar.output("generated/config.h")},
  lang = {
    c = {
      include_dirs = {"generated"},
    },
  },
}
EOF
"$qstar" --file "$tmp/fanout/qstar.lua" build //:app --jobs 2 --schedule-trace > "$tmp/fanout-build.out" 2> "$tmp/fanout-build.err"
contains "$tmp/fanout-build.out" "build_action id=//:cfg:generate:0 status=run"
contains "$tmp/fanout-build.out" "parallel_compile target=//:app jobs=2 sources=3 mode=process-v2"
contains "$tmp/fanout-build.out" "parallel_slot target=//:app slot=0 state=assign action=//:app:compile:0 queue=0"
contains "$tmp/fanout-build.out" "parallel_slot target=//:app slot=1 state=assign action=//:app:compile:1 queue=1"
contains "$tmp/fanout-build.out" "action=//:app:compile:2 queue=2"
contains "$tmp/fanout-build.out" "status ok"

mkdir -p "$tmp/parallel-fail/src" "$tmp/parallel-fail/tools"
cat > "$tmp/parallel-fail/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
src=
out=
dep=
prev=
for arg in "$@"; do
  if [ "$prev" = "-o" ]; then out=$arg; prev=; continue; fi
  if [ "$prev" = "-MF" ]; then dep=$arg; prev=; continue; fi
  case "$arg" in
    -o) prev="-o" ;;
    -MF) prev="-MF" ;;
    *.c) src=$arg ;;
  esac
done
case "$src" in
  *slow.c) sleep 20 ;;
  *fail.c) echo "fake compiler failure" >&2; exit 9 ;;
esac
if [ "$dep" ]; then
  mkdir -p "$(dirname "$dep")"
  printf '%s: %s\n' "$out" "$src" > "$dep"
fi
cc -c "$src" -o "$out"
EOF
chmod +x "$tmp/parallel-fail/tools/fake-cc.sh"
cat > "$tmp/parallel-fail/src/slow.c" <<'EOF'
int slow_value(void) { return 1; }
EOF
cat > "$tmp/parallel-fail/src/fail.c" <<'EOF'
int fail_value(void) { return 2; }
EOF
cat > "$tmp/parallel-fail/src/ok.c" <<'EOF'
int ok_value(void) { return 3; }
EOF
cat > "$tmp/parallel-fail/Cale.toml" <<'EOF'
profile = "fake"

[profile.fake]
cc = "tools/fake-cc.sh"
EOF
cat > "$tmp/parallel-fail/qstar.lua" <<'EOF'
qstar.executable "race" {
  sources = {"src/slow.c", "src/fail.c", "src/ok.c"},
}
EOF
if "$qstar" --file "$tmp/parallel-fail/qstar.lua" build //:race --jobs 2 --schedule-trace > "$tmp/parallel-fail.out" 2> "$tmp/parallel-fail.err"; then
	fail "parallel failure unexpectedly succeeded"
fi
contains "$tmp/parallel-fail.out" "parallel_compile target=//:race jobs=2 sources=3 mode=process-v2"
contains "$tmp/parallel-fail.out" "parallel_batch target=//:race jobs=2 total=3 policy=fifo"
contains "$tmp/parallel-fail.out" "parallel_slot target=//:race slot=0 state=assign action=//:race:compile:0 queue=0"
contains "$tmp/parallel-fail.out" "parallel_slot target=//:race slot=1 state=assign action=//:race:compile:1 queue=1"
contains "$tmp/parallel-fail.out" "build_action id=//:race:compile:1 status=fail exit=9"
contains "$tmp/parallel-fail.out" "parallel_event target=//:race event=fail id=//:race:compile:1 slot=1 exit=9 state=failed retry=next-build cancel=active"
contains "$tmp/parallel-fail.out" "build_action id=//:race:compile:0 status=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-fail.out" "parallel_event target=//:race event=cancel id=//:race:compile:0 slot=0 state=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-fail/.qstar/logs/___race_compile_1.log" "qstar-action-log v2"
contains "$tmp/parallel-fail/.qstar/logs/___race_compile_1.log" "argv[0]=tools/fake-cc.sh"
contains "$tmp/parallel-fail/.qstar/logs/last-failure.replay" "argv_digest="

mkdir -p "$tmp/parallel-timeout/src" "$tmp/parallel-timeout/tools"
cat > "$tmp/parallel-timeout/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
sleep 5
EOF
chmod +x "$tmp/parallel-timeout/tools/fake-cc.sh"
cat > "$tmp/parallel-timeout/src/timeout.c" <<'EOF'
int timeout_value(void) { return 1; }
EOF
cat > "$tmp/parallel-timeout/src/other.c" <<'EOF'
int other_value(void) { return 2; }
EOF
cat > "$tmp/parallel-timeout/Cale.toml" <<'EOF'
profile = "fake"

[profile.fake]
cc = "tools/fake-cc.sh"
EOF
cat > "$tmp/parallel-timeout/qstar.lua" <<'EOF'
qstar.executable "timeout" {
  sources = {"src/timeout.c", "src/other.c"},
}
EOF
if QSTAR_TEST_ACTION_TIMEOUT_SEC=1 "$qstar" --file "$tmp/parallel-timeout/qstar.lua" build //:timeout --jobs 2 --schedule-trace > "$tmp/parallel-timeout.out" 2> "$tmp/parallel-timeout.err"; then
	fail "parallel timeout unexpectedly succeeded"
fi
contains "$tmp/parallel-timeout.out" "executor-policy version=v3 parallel=optional jobs=2 active=compile-process-v2 failure=stop-on-first-failure action_timeout_sec=1"
contains "$tmp/parallel-timeout.out" "parallel_compile target=//:timeout jobs=2 sources=2 mode=process-v2"
contains "$tmp/parallel-timeout.out" "parallel_event target=//:timeout event=timeout id=//:timeout:compile:0 slot=0 state=timeout retry=next-build cancel=active"
contains "$tmp/parallel-timeout.out" "build_action id=//:timeout:compile:1 status=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-timeout.out" "parallel_event target=//:timeout event=cancel id=//:timeout:compile:1 slot=1 state=cancelled reason=parallel-failure retry=next-build"
contains "$tmp/parallel-timeout.out" "cancel_propagation policy=stop-on-first-failure"
contains "$tmp/parallel-timeout/.qstar/logs/last-failure.replay" "argv_digest="

cp -R "$project_root/generated-config" "$tmp/project-generated"
"$qstar" --file "$tmp/project-generated/qstar.lua" build //:app > "$tmp/project-generated-build.out" 2> "$tmp/project-generated-build.err"
contains "$tmp/project-generated-build.out" "status ok"
test -f "$tmp/project-generated/generated/config.h" || fail "project corpus generated config missing"
test -f "$tmp/project-generated/generated/value.c" || fail "project corpus generated source missing"
"$tmp/project-generated/.qstar/out/___app/app"
contains "$tmp/project-generated/compile_commands.json" "generated/value.c"
contains "$tmp/project-generated/.qstar/state/graph.json" "\"schema\":\"qstar-graph-snapshot-v1\""
"$qstar" --file "$tmp/project-generated/qstar.lua" build //:app > "$tmp/project-generated-skip.out" 2> "$tmp/project-generated-skip.err"
contains "$tmp/project-generated-skip.out" "status=skip"

cp -R "$project_root/binary-blob-embed" "$tmp/project-blob"
"$qstar" --file "$tmp/project-blob/qstar.lua" build //:probe --explain-cache > "$tmp/project-blob-build.out" 2> "$tmp/project-blob-build.err"
contains "$tmp/project-blob-build.out" "build_action id=//:embed_object:generate:0 status=run"
contains "$tmp/project-blob-build.out" "build_action id=//:probe:link:0 status=run"
contains "$tmp/project-blob-build.out" "status ok"
"$tmp/project-blob/.qstar/out/___probe/probe"
"$qstar" --file "$tmp/project-blob/qstar.lua" build //:probe --explain-cache > "$tmp/project-blob-skip.out" 2> "$tmp/project-blob-skip.err"
contains "$tmp/project-blob-skip.out" "build_action id=//:embed_object:generate:0 status=skip"
contains "$tmp/project-blob-skip.out" "build_action id=//:probe:link:0 status=skip"
printf 'payload-v2\n' >> "$tmp/project-blob/fixtures/payload.elf"
"$qstar" --file "$tmp/project-blob/qstar.lua" build //:probe --explain-cache > "$tmp/project-blob-rebuild.out" 2> "$tmp/project-blob-rebuild.err"
contains "$tmp/project-blob-rebuild.out" "cache_miss id=//:embed_object:generate:0 reason=input-changed"
contains "$tmp/project-blob-rebuild.out" "cache_miss id=//:probe:link:0 reason=input-changed"

cp -R "$project_root/multipkg" "$tmp/project-multipkg"
"$qstar" --file "$tmp/project-multipkg/qstar.lua" build //app:app > "$tmp/project-multipkg-build.out" 2> "$tmp/project-multipkg-build.err"
contains "$tmp/project-multipkg-build.out" "package-root $tmp/project-multipkg"
contains "$tmp/project-multipkg-build.out" "status ok"
"$tmp/project-multipkg/.qstar/out/__app_app/app"
"$qstar" --file "$tmp/project-multipkg/qstar.lua" install //lib:core --prefix "$tmp/project-multipkg-prefix" > "$tmp/project-multipkg-install.out" 2> "$tmp/project-multipkg-install.err"
test -f "$tmp/project-multipkg-prefix/lib/libcore.a" || fail "multipkg corpus lib did not install"
test -f "$tmp/project-multipkg-prefix/include/core.h" || fail "multipkg corpus header did not install"
contains "$tmp/project-multipkg/.qstar/install/manifest.json" "\"schema\":\"qstar-install-manifest-v2\""
contains "$tmp/project-multipkg/compile_commands.json" "lib/src/core.c"
contains "$tmp/project-multipkg/compile_commands.json" "app/src/main.c"

cp -R "$project_root/source-dir-style" "$tmp/project-source-dir"
"$qstar" --file "$tmp/project-source-dir/qstar.lua" lint //... > "$tmp/project-source-dir-lint.out" 2> "$tmp/project-source-dir-lint.err"
contains "$tmp/project-source-dir-lint.out" "status ok"
"$qstar" --file "$tmp/project-source-dir/qstar.lua" explain //app/src:app > "$tmp/project-source-dir-explain.out" 2> "$tmp/project-source-dir-explain.err"
contains "$tmp/project-source-dir-explain.out" "closure-order [//lib/src:core, //app/src:app]"
"$qstar" --file "$tmp/project-source-dir/qstar.lua" dry-run //app/src:app > "$tmp/project-source-dir-dry.out" 2> "$tmp/project-source-dir-dry.err"
contains "$tmp/project-source-dir-dry.out" "dry_run_target //app/src:app"
"$qstar" --file "$tmp/project-source-dir/qstar.lua" build //app/src:app > "$tmp/project-source-dir-build.out" 2> "$tmp/project-source-dir-build.err"
contains "$tmp/project-source-dir-build.out" "status ok"
"$tmp/project-source-dir/.qstar/out/__app_src_app/app"
contains "$tmp/project-source-dir/compile_commands.json" "lib/src/core.c"
contains "$tmp/project-source-dir/compile_commands.json" "app/src/main.c"

contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/c-only"
contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/generated"
contains "../docs/qstar/qstar-v0-seal.md" "qstar/tests/manual/mixed-cale"
contains "../docs/qstar/qstar-v0-seal.md" "make -C qstar qstar-v0-release-tests"
contains "../docs/qstar/qstar-v0-seal.md" "qstar-v0.1-release-tests"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "status: v0.1 standalone developer build system"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "make -C qstar qstar-v0.1-release-tests"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/c-app-lib-test"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/cxx-mixed"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/generated-config"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar/tests/projects/multipkg"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "Cale build integration: deferred"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar action-log <action-id>"
contains "../docs/qstar/qstar-v0.1-hardening-seal.md" "qstar replay <action-id>"
contains "README.md" "docs/qstar/qstar-v0-seal.md"
contains "README.md" "docs/qstar/qstar-v0.1-hardening-seal.md"
contains "README.md" "qstar-v0.1-release-tests"

"$qstar" init c-app "$tmp/init-c-app" > "$tmp/init-c-app.out" 2> "$tmp/init-c-app.err"
contains "$tmp/init-c-app.out" "qstar init v1"
contains "$tmp/init-c-app.out" "template c-app"
"$qstar" --file "$tmp/init-c-app/qstar.lua" build //:app > "$tmp/init-c-app-build.out" 2> "$tmp/init-c-app-build.err"
contains "$tmp/init-c-app-build.out" "status ok"
"$tmp/init-c-app/.qstar/out/___app/app"

"$qstar" init c-lib "$tmp/init-c-lib" > "$tmp/init-c-lib.out" 2> "$tmp/init-c-lib.err"
contains "$tmp/init-c-lib.out" "template c-lib"
"$qstar" --file "$tmp/init-c-lib/qstar.lua" test //:unit > "$tmp/init-c-lib-test.out" 2> "$tmp/init-c-lib-test.err"
contains "$tmp/init-c-lib-test.out" "test_result label=//:unit status=pass"
"$qstar" --file "$tmp/init-c-lib/qstar.lua" install //:core --prefix "$tmp/init-c-lib-prefix" > "$tmp/init-c-lib-install.out" 2> "$tmp/init-c-lib-install.err"
test -f "$tmp/init-c-lib-prefix/lib/libcore.a" || fail "init c-lib did not install static library"

"$qstar" init generated "$tmp/init-generated" > "$tmp/init-generated.out" 2> "$tmp/init-generated.err"
contains "$tmp/init-generated.out" "template generated"
"$qstar" --file "$tmp/init-generated/qstar.lua" build //:app > "$tmp/init-generated-build.out" 2> "$tmp/init-generated-build.err"
contains "$tmp/init-generated-build.out" "status ok"
test -f "$tmp/init-generated/generated/config.h" || fail "init generated missing config header"
"$tmp/init-generated/.qstar/out/___app/app"

"$qstar" init mixed-cale "$tmp/init-mixed" > "$tmp/init-mixed.out" 2> "$tmp/init-mixed.err"
contains "$tmp/init-mixed.out" "template mixed-cale"
"$qstar" --file "$tmp/init-mixed/qstar.lua" dry-run //:mixed > "$tmp/init-mixed-dry.out" 2> "$tmp/init-mixed-dry.err"
contains "$tmp/init-mixed-dry.out" "argv=[cale, -c, src/plugin.cale"

if "$qstar" init c-app "$tmp/init-c-app" > "$tmp/init-overwrite.out" 2> "$tmp/init-overwrite.err"; then
	fail "qstar init unexpectedly overwrote existing files"
fi
contains "$tmp/init-overwrite.err" "refuses to overwrite existing file"

"$qstar" --file "$tmp/init-c-lib/qstar.lua" explain //:core > "$tmp/rule-explain.out" 2> "$tmp/rule-explain.err"
contains "$tmp/rule-explain.out" "rule provider=native final_action=archive output_group=libs"
contains "$tmp/rule-explain.out" "source_file path=src/core.c language=c tool=c-compiler provider=c output_group=objects role=compile"

mkdir -p "$tmp/depfile/include" "$tmp/depfile/src"
cat > "$tmp/depfile/include/dep.h" <<'EOF'
#define DEP_VALUE 11
EOF
cat > "$tmp/depfile/src/main.c" <<'EOF'
#include "dep.h"
int main(void) { return DEP_VALUE - 11; }
EOF
cat > "$tmp/depfile/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {"include"},
    },
  },
}
EOF
"$qstar" --file "$tmp/depfile/qstar.lua" build //:app > "$tmp/depfile-first.out" 2> "$tmp/depfile-first.err"
contains "$tmp/depfile-first.out" "status ok"
test -f "$tmp/depfile/.qstar/out/___app/obj0.d" || fail "missing compiler depfile"
cat > "$tmp/depfile/include/dep.h" <<'EOF'
#define DEP_VALUE 12
EOF
"$qstar" --file "$tmp/depfile/qstar.lua" build //:app --explain-cache > "$tmp/depfile-second.out" 2> "$tmp/depfile-second.err"
contains "$tmp/depfile-second.out" "cache_miss id=//:app:compile:0"
contains "$tmp/depfile-second.out" "reason=depfile-changed"
rm -f "$tmp/depfile/include/dep.h"
if "$qstar" --file "$tmp/depfile/qstar.lua" build //:app > "$tmp/depfile-missing.out" 2> "$tmp/depfile-missing.err"; then
	fail "missing depfile-discovered header unexpectedly succeeded"
fi
contains "$tmp/depfile-missing.err" "depfile-discovered header"

if command -v c++ >/dev/null 2>&1; then
	mkdir -p "$tmp/cxx/include" "$tmp/cxx/src"
	cat > "$tmp/cxx/include/cpp.hpp" <<'EOF'
#ifndef QSTAR_CXX_FLAG
#error missing C++ flag
#endif
static inline int qstar_cpp_value(void) { return 37 + QSTAR_CXX_FLAG; }
EOF
	cat > "$tmp/cxx/src/cpp.cpp" <<'EOF'
#include "cpp.hpp"
extern "C" int cpp_value(void) { return qstar_cpp_value(); }
EOF
	cat > "$tmp/cxx/src/main.c" <<'EOF'
#ifndef QSTAR_C_FLAG
#error missing C flag
#endif
int cpp_value(void);
int main(void) { return cpp_value() - 42; }
EOF
	cat > "$tmp/cxx/qstar.lua" <<'EOF'
qstar.executable "mixed" {
  sources = {"src/main.c", "src/cpp.cpp"},
  lang = {
    c = {
      include_dirs = {"include"},
      compile_options = {"-DQSTAR_C_FLAG=1"},
    },
    cxx = {
      include_dirs = {"include"},
      compile_options = {"-DQSTAR_CXX_FLAG=5"},
      standard = "c++11",
    },
  },
}
EOF
	"$qstar" --file "$tmp/cxx/qstar.lua" dry-run //:mixed > "$tmp/cxx-dry.out" 2> "$tmp/cxx-dry.err"
	contains "$tmp/cxx-dry.out" "argv=[c++, -c, src/cpp.cpp"
	contains "$tmp/cxx-dry.out" "-std=c++11"
	contains "$tmp/cxx-dry.out" "-DQSTAR_CXX_FLAG=5"
	contains "$tmp/cxx-dry.out" "argv=[c++, -o, .qstar/out/___mixed/mixed"
	"$qstar" --file "$tmp/cxx/qstar.lua" build //:mixed > "$tmp/cxx-build.out" 2> "$tmp/cxx-build.err"
	contains "$tmp/cxx-build.out" "status ok"
	"$tmp/cxx/.qstar/out/___mixed/mixed"
	contains "$tmp/cxx/compile_commands.json" "src/cpp.cpp"
fi

mkdir -p "$tmp/asm/asm/include" "$tmp/asm/src"
cat > "$tmp/asm/asm/include/asm_value.inc" <<'EOF'
#define QSTAR_ASM_RETURN QSTAR_ASM_VALUE
EOF
cat > "$tmp/asm/asm/value.S" <<'EOF'
#include "asm_value.inc"
#if defined(__aarch64__) || defined(__arm64__)
#if defined(__APPLE__)
.globl _asm_value
.p2align 2
_asm_value:
#else
.globl asm_value
.p2align 2
asm_value:
#endif
	mov w0, #QSTAR_ASM_RETURN
	ret
#elif defined(__x86_64__)
#if defined(__APPLE__)
.globl _asm_value
_asm_value:
#else
.globl asm_value
asm_value:
#endif
	movl $QSTAR_ASM_RETURN, %eax
	ret
#else
#error unsupported qstar asm smoke architecture
#endif
EOF
cat > "$tmp/asm/asm/empty.s" <<'EOF'
.text
EOF
cat > "$tmp/asm/src/main.c" <<'EOF'
int asm_value(void);
int main(void) { return asm_value() == 42 ? 0 : 1; }
EOF
cat > "$tmp/asm/qstar.lua" <<'EOF'
qstar.staticlib "plainasm" {
  sources = {"asm/empty.s"},
}

qstar.executable "asmapp" {
  sources = {"src/main.c", "asm/value.S"},
  lang = {
    asm = {
      include_dirs = {"asm/include"},
      compile_options = {"-DQSTAR_ASM_VALUE=42"},
      preprocess = true,
    },
  },
}

qstar.executable "bad_asm_toolchain" {
  toolchain = "cale",
  sources = {"asm/value.S"},
}
EOF
"$qstar" --file "$tmp/asm/qstar.lua" dry-run //:asmapp > "$tmp/asm-dry.out" 2> "$tmp/asm-dry.err"
contains "$tmp/asm-dry.out" "language=asm-cpp"
contains "$tmp/asm-dry.out" "argv=[cc, -x, assembler-with-cpp, -c, asm/value.S"
contains "$tmp/asm-dry.out" "-DQSTAR_ASM_VALUE=42"
contains "$tmp/asm-dry.out" "asm/include"
"$qstar" --file "$tmp/asm/qstar.lua" build //:plainasm > "$tmp/asm-plain-build.out" 2> "$tmp/asm-plain-build.err"
contains "$tmp/asm-plain-build.out" "status ok"
"$qstar" --file "$tmp/asm/qstar.lua" build //:asmapp > "$tmp/asm-build.out" 2> "$tmp/asm-build.err"
contains "$tmp/asm-build.out" "status ok"
"$tmp/asm/.qstar/out/___asmapp/asmapp"
contains "$tmp/asm/compile_commands.json" "asm/value.S"
contains "$tmp/asm/compile_commands.json" "-x assembler-with-cpp"
if "$qstar" --file "$tmp/asm/qstar.lua" build //:bad_asm_toolchain > "$tmp/asm-bad-toolchain.out" 2> "$tmp/asm-bad-toolchain.err"; then
	fail "assembler with Cale toolchain unexpectedly succeeded"
fi
contains "$tmp/asm-bad-toolchain.err" "assembler source 'asm/value.S' requires host or clang toolchain"

cat > "$tmp/cxx-module.qstar.lua" <<'EOF'
qstar.executable "bad_module" {
  sources = {"src/module.cppm"},
}
EOF
cat > "$tmp/src/module.cppm" <<'EOF'
export module bad;
EOF
if "$qstar" --file "$tmp/cxx-module.qstar.lua" build //:bad_module > "$tmp/cxx-module.out" 2> "$tmp/cxx-module.err"; then
	fail "C++ module source unexpectedly built"
fi
contains "$tmp/cxx-module.err" "C++ modules are not supported"

mkdir -p "$tmp/lang-surface/boot/include" "$tmp/lang-surface/src" "$tmp/lang-surface/include"
cat > "$tmp/lang-surface/qstar.lua" <<'EOF'
qstar.staticlib "boot" {
  sources = {"boot/start.S"},
  lang = {
    asm = {
      include_dirs = {"boot/include"},
      compile_options = {"-ffreestanding"},
      preprocess = true,
    },
  },
}

qstar.staticlib "cale_core" {
  toolchain = "cale",
  sources = {"src/core.cl"},
  lang = {
    cale = {
      profile = "safe",
      compile_options = {"--profile=safe"},
      hcl_include_dirs = {"include"},
    },
  },
}
EOF
cat > "$tmp/lang-surface/boot/start.S" <<'EOF'
.globl _start
_start:
EOF
cat > "$tmp/lang-surface/src/core.cl" <<'EOF'
fn core() -> int { return 0; }
EOF
"$qstar" --file "$tmp/lang-surface/qstar.lua" --dump-graph > "$tmp/lang-surface.out" 2> "$tmp/lang-surface.err"
contains "$tmp/lang-surface.out" "lang.asm.include_dirs [boot/include]"
contains "$tmp/lang-surface.out" "lang.asm.compile_options [-ffreestanding]"
contains "$tmp/lang-surface.out" "lang.asm.preprocess true"
contains "$tmp/lang-surface.out" "lang.cale.hcl_include_dirs [include]"
contains "$tmp/lang-surface.out" "lang.cale.compile_options [--profile=safe]"
contains "$tmp/lang-surface.out" "lang.cale.profile safe"

mkdir -p "$tmp/workspace/app/src" "$tmp/workspace/lib/src" "$tmp/workspace/lib/include" "$tmp/workspace/lib/private"
touch "$tmp/workspace/qstar.workspace"
cat > "$tmp/workspace/lib/include/core.h" <<'EOF'
int core_value(void);
EOF
cat > "$tmp/workspace/lib/private/core_private.h" <<'EOF'
#define CORE_PRIVATE 1
EOF
cat > "$tmp/workspace/lib/src/core.c" <<'EOF'
#include "core.h"
int core_value(void) { return 5; }
EOF
cat > "$tmp/workspace/app/src/main.c" <<'EOF'
#include "core.h"
int main(void) { return core_value() - 5; }
EOF
cat > "$tmp/workspace/lib/lib.qs" <<'EOF'
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  public_headers = {"lib/include/core.h"},
  private_headers = {"lib/private/core_private.h"},
  lang = {
    c = {
      public_include_dirs = {"lib/include"},
      private_include_dirs = {"lib/private"},
    },
  },
  visibility = {"//app:..."},
}
EOF
cat > "$tmp/workspace/app/app.qs" <<'EOF'
qstar.executable "app" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
}
EOF
cat > "$tmp/workspace/qstar.lua" <<'EOF'
qstar.subdir("lib")
qstar.subdir("app")
EOF
"$qstar" --file "$tmp/workspace/app/app.qs" query //app:app > "$tmp/workspace-query.out" 2> "$tmp/workspace-query.err"
contains "$tmp/workspace-query.out" "target //app:app"
contains "$tmp/workspace-query.out" "package app"
"$qstar" --file "$tmp/workspace/qstar.lua" build //app:app > "$tmp/workspace-build.out" 2> "$tmp/workspace-build.err"
contains "$tmp/workspace-build.out" "package-root $tmp/workspace"
contains "$tmp/workspace-build.out" "status ok"
"$tmp/workspace/.qstar/out/__app_app/app"

cat > "$tmp/workspace/app/app.qs" <<'EOF'
qstar.executable "bad_leak" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
  lang = {
    c = {
      include_dirs = {"lib/private"},
    },
  },
}
EOF
if "$qstar" --file "$tmp/workspace/qstar.lua" check //app:bad_leak > "$tmp/private-leak.out" 2> "$tmp/private-leak.err"; then
	fail "private include leakage unexpectedly succeeded"
fi
contains "$tmp/private-leak.err" "leaks private include directory"
if "$qstar" --file "$tmp/workspace/qstar.lua" lint //app:bad_leak > "$tmp/private-leak-lint.out" 2> "$tmp/private-leak-lint.err"; then
	fail "private include leakage lint unexpectedly succeeded"
fi
contains "$tmp/private-leak-lint.out" "QSTAR030"
contains "$tmp/private-leak-lint.out" "leaks private include directory"

cat > "$tmp/workspace/lib/lib.qs" <<'EOF'
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  public_headers = {"lib/include/core.h"},
  lang = {
    c = {
      public_include_dirs = {"lib/include"},
    },
  },
  visibility = {"//other:..."},
}
EOF
cat > "$tmp/workspace/app/app.qs" <<'EOF'
qstar.executable "blocked" {
  sources = {"app/src/main.c"},
  deps = {"//lib:core"},
}
EOF
if "$qstar" --file "$tmp/workspace/qstar.lua" check //app:blocked > "$tmp/visibility.out" 2> "$tmp/visibility.err"; then
	fail "visibility violation unexpectedly succeeded"
fi
contains "$tmp/visibility.err" "is not visible"

cat > "$tmp/workspace/app/app.qs" <<'EOF'
qstar.executable "//other:oops" {
  sources = {"app/src/main.c"},
}
EOF
if "$qstar" --file "$tmp/workspace/app/app.qs" check //other:oops > "$tmp/ownership.out" 2> "$tmp/ownership.err"; then
	fail "cross-package ownership unexpectedly succeeded"
fi
contains "$tmp/ownership.err" "owned by package"

cat > "$tmp/workspace/app/app.qs" <<'EOF'
qstar.executable "outside" {
  sources = {"../outside.c"},
}
EOF
if "$qstar" --file "$tmp/workspace/app/app.qs" check //app:outside > "$tmp/outside-source.out" 2> "$tmp/outside-source.err"; then
	fail "outside source path unexpectedly succeeded"
fi
contains "$tmp/outside-source.err" "must be package-relative"

mkdir -p "$tmp/profile/.cale/profiles" "$tmp/profile/src"
cat > "$tmp/profile/Cale.toml" <<'EOF'
profile = "custom"

[profile.custom]
toolchain = "clang"
target = "x86_64-unknown-none-elf"
stdlib = "none"
cc = "clang-custom"
cxx = "clang++-custom"
cale = "cale-custom"
ar = "llvm-ar-custom"
linker = "ld-custom"
sysroot = "sdk root"
resource_dir = "resource dir"
include_dirs = ["profile include", "profinc"]
lib_dirs = ["profile lib"]
EOF
cat > "$tmp/profile/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/profile/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  libs = {"m"},
}
EOF
"$qstar" --file "$tmp/profile/qstar.lua" dry-run //:app > "$tmp/profile-dry.out" 2> "$tmp/profile-dry.err"
contains "$tmp/profile-dry.out" "resolved_toolchain owner=//:app toolchain=clang profile=custom target=x86_64-unknown-none-elf stdlib=none resolver=profile-schema-v2 cc=clang-custom"
contains "$tmp/profile-dry.out" "\"--sysroot=sdk root\""
contains "$tmp/profile-dry.out" "-resource-dir"
contains "$tmp/profile-dry.out" "\"resource dir\""
contains "$tmp/profile-dry.out" "\"profile include\""
contains "$tmp/profile-dry.out" "\"-Lprofile lib\""
contains "$tmp/profile-dry.out" "digest="
"$qstar" --file "$tmp/profile/qstar.lua" doctor > "$tmp/profile-doctor.out" 2> "$tmp/profile-doctor.err"
contains "$tmp/profile-doctor.out" "profile-schema v2 include_dirs=2 lib_dirs=1"
contains "$tmp/profile-doctor.out" "toolchain-sanity name=clang cc=clang-custom cxx=clang++-custom cale=cale-custom ar=llvm-ar-custom linker=ld-custom"

mkdir -p "$tmp/freestanding/src" "$tmp/freestanding/tools" "$tmp/freestanding/linker"
cat > "$tmp/freestanding/tools/fake-cc.sh" <<'EOF'
#!/bin/sh
set -eu
out=
dep=
src=
while [ $# -gt 0 ]; do
	case "$1" in
	-o)
		shift
		out=$1
		;;
	-MF)
		shift
		dep=$1
		;;
	-c)
		shift
		src=$1
		;;
	esac
	shift || break
done
mkdir -p "$(dirname "$out")"
printf "fake object\n" > "$out"
if [ -n "$dep" ]; then
	mkdir -p "$(dirname "$dep")"
	printf "%s: %s\n" "$out" "$src" > "$dep"
fi
EOF
cat > "$tmp/freestanding/tools/fake-link.sh" <<'EOF'
#!/bin/sh
set -eu
out=
while [ $# -gt 0 ]; do
	if [ "$1" = "-o" ]; then
		shift
		out=$1
	fi
	shift || break
done
mkdir -p "$(dirname "$out")"
printf "fake link\n" > "$out"
EOF
chmod +x "$tmp/freestanding/tools/fake-cc.sh" "$tmp/freestanding/tools/fake-link.sh"
cat > "$tmp/freestanding/src/kernel.c" <<'EOF'
int kernel_main(void) { return 0; }
EOF
cat > "$tmp/freestanding/linker/profile.ld" <<'EOF'
SECTIONS { . = 0x1000; }
EOF
cat > "$tmp/freestanding/linker/kernel.ld" <<'EOF'
SECTIONS { . = 0x80000; }
EOF
cat > "$tmp/freestanding/Cale.toml" <<'EOF'
profile = "kernel"

[profile.kernel]
toolchain = "clang"
target = "aarch64-none-elf"
arch = "aarch64"
cpu = "cortex-a76"
abi = "lp64"
freestanding = true
cc = "tools/fake-cc.sh"
linker = "tools/fake-link.sh"
linker_script = "linker/profile.ld"
link_options = ["-nostdlib"]
defsyms = ["__profile_base=0x1000"]
EOF
cat > "$tmp/freestanding/qstar.lua" <<'EOF'
qstar.executable "kernel" {
  sources = {"src/kernel.c"},
  link_options = {"-Wl,-Map=kernel.map"},
  linker_script = "linker/kernel.ld",
  defsyms = {"__stack_top=0x80000"},
}
EOF
"$qstar" --file "$tmp/freestanding/qstar.lua" dry-run //:kernel > "$tmp/freestanding-dry.out" 2> "$tmp/freestanding-dry.err"
contains "$tmp/freestanding-dry.out" "profile_target arch=aarch64 cpu=cortex-a76 abi=lp64 freestanding=true"
contains "$tmp/freestanding-dry.out" "profile_link linker_script=linker/profile.ld link_options=[-nostdlib] defsyms=[__profile_base=0x1000]"
contains "$tmp/freestanding-dry.out" "-ffreestanding"
contains "$tmp/freestanding-dry.out" "-fno-builtin"
contains "$tmp/freestanding-dry.out" "-fno-stack-protector"
contains "$tmp/freestanding-dry.out" "-mgeneral-regs-only"
contains "$tmp/freestanding-dry.out" "-mcpu=cortex-a76"
contains "$tmp/freestanding-dry.out" "-mabi=lp64"
contains "$tmp/freestanding-dry.out" "-nostdlib"
contains "$tmp/freestanding-dry.out" "-Wl,-Map=kernel.map"
contains "$tmp/freestanding-dry.out" "-T"
contains "$tmp/freestanding-dry.out" "linker/kernel.ld"
contains "$tmp/freestanding-dry.out" "--defsym=__profile_base=0x1000"
contains "$tmp/freestanding-dry.out" "--defsym=__stack_top=0x80000"
"$qstar" --file "$tmp/freestanding/qstar.lua" build //:kernel > "$tmp/freestanding-build.out" 2> "$tmp/freestanding-build.err"
contains "$tmp/freestanding-build.out" "status ok"
contains "$tmp/freestanding/.qstar/logs/___kernel_compile_0.log" "-ffreestanding"
contains "$tmp/freestanding/.qstar/logs/___kernel_compile_0.log" "-mgeneral-regs-only"
contains "$tmp/freestanding/.qstar/logs/___kernel_link_0.log" "-T linker/kernel.ld"
contains "$tmp/freestanding/.qstar/logs/___kernel_link_0.log" "--defsym=__stack_top=0x80000"
cat > "$tmp/freestanding/linker/kernel.ld" <<'EOF'
SECTIONS { . = 0x90000; }
EOF
"$qstar" --file "$tmp/freestanding/qstar.lua" build //:kernel --explain-cache > "$tmp/freestanding-rebuild.out" 2> "$tmp/freestanding-rebuild.err"
contains "$tmp/freestanding-rebuild.out" "cache_miss id=//:kernel:link:0"
contains "$tmp/freestanding-rebuild.out" "reason=input-changed"
contains "$tmp/freestanding-rebuild.out" "status=skip"
cat > "$tmp/freestanding/qstar.lua" <<'EOF'
qstar.executable "bad_script" {
  sources = {"src/kernel.c"},
  linker_script = "../escape.ld",
}
EOF
if "$qstar" --file "$tmp/freestanding/qstar.lua" check //:bad_script > "$tmp/freestanding-bad-script.out" 2> "$tmp/freestanding-bad-script.err"; then
	fail "package-escaping linker_script unexpectedly succeeded"
fi
contains "$tmp/freestanding-bad-script.err" "linker_script '../escape.ld' in '//:bad_script' must be package-relative"
cat > "$tmp/freestanding/qstar.lua" <<'EOF'
qstar.executable "bad_defsym" {
  sources = {"src/kernel.c"},
  defsyms = {"BROKEN"},
}
EOF
if "$qstar" --file "$tmp/freestanding/qstar.lua" check //:bad_defsym > "$tmp/freestanding-bad-defsym.out" 2> "$tmp/freestanding-bad-defsym.err"; then
	fail "bad defsym unexpectedly succeeded"
fi
contains "$tmp/freestanding-bad-defsym.err" "defsym 'BROKEN' in '//:bad_defsym' must be NAME=VALUE"
cat > "$tmp/freestanding/Cale.toml" <<'EOF'
profile = "missing"

[profile.missing]
toolchain = "clang"
target = "aarch64-none-elf"
linker_script = "linker/missing.ld"
EOF
cat > "$tmp/freestanding/qstar.lua" <<'EOF'
qstar.executable "missing_profile_script" {
  sources = {"src/kernel.c"},
}
EOF
if "$qstar" --file "$tmp/freestanding/qstar.lua" check //:missing_profile_script > "$tmp/freestanding-missing-profile.out" 2> "$tmp/freestanding-missing-profile.err"; then
	fail "missing profile linker script unexpectedly succeeded"
fi
contains "$tmp/freestanding-missing-profile.err" "profile linker_script 'linker/missing.ld' does not exist"

mkdir -p "$tmp/exttool/bin" "$tmp/exttool/src" "$tmp/exttool/tools"
cat > "$tmp/exttool/bin/qstar-extgen" <<'EOF'
#!/bin/sh
set -eu
out=$1
mkdir -p "$(dirname "$out")"
printf 'int ext_value(void) { return 3; }\n' > "$out"
EOF
chmod +x "$tmp/exttool/bin/qstar-extgen"
cat > "$tmp/exttool/src/main.c" <<'EOF'
int ext_value(void);
int main(void) { return ext_value() - 3; }
EOF
cat > "$tmp/exttool/Cale.toml" <<'EOF'
profile = "tools"

[profile.tools]
path_tools = ["qstar-extgen"]
EOF
cat > "$tmp/exttool/qstar.lua" <<'EOF'
qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/ext.c")},
  command = qstar.cli {"qstar-extgen", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/ext.c"),
  },
}
EOF
PATH="$tmp/exttool/bin:$PATH" "$qstar" --file "$tmp/exttool/qstar.lua" doctor > "$tmp/exttool-doctor.out" 2> "$tmp/exttool-doctor.err"
contains "$tmp/exttool-doctor.out" "profile_external_tools allow_absolute=false path_tools=[qstar-extgen] tool_overrides=[]"
contains "$tmp/exttool-doctor.out" "external-tool-policy path_tools=1 tool_overrides=0 allow_absolute=false"
contains "$tmp/exttool-doctor.out" "external-tool name=qstar-extgen mode=path status=found"
PATH="$tmp/exttool/bin:$PATH" "$qstar" --file "$tmp/exttool/qstar.lua" dry-run //:app > "$tmp/exttool-dry.out" 2> "$tmp/exttool-dry.err"
contains "$tmp/exttool-dry.out" "dry_run_step id=//:generated_ext:generate:0"
contains "$tmp/exttool-dry.out" "tool=qstar-extgen tool_mode=path resolved_tool=qstar-extgen"
contains "$tmp/exttool-dry.out" "command_argv id=//:generated_ext:generate:0"
PATH="$tmp/exttool/bin:$PATH" "$qstar" --file "$tmp/exttool/qstar.lua" build //:app > "$tmp/exttool-build.out" 2> "$tmp/exttool-build.err"
contains "$tmp/exttool-build.out" "generated_sandbox id=//:generated_ext inputs=package-root outputs=generated-only cwd=package-root network=disabled tool=qstar-extgen tool_mode=path resolved_tool=qstar-extgen"
contains "$tmp/exttool-build.out" "status ok"
contains "$tmp/exttool/.qstar/logs/___generated_ext_generate_0.log" "argv[0]=qstar-extgen"
"$tmp/exttool/.qstar/out/___app/app"

mkdir -p "$tmp/exttool-deny/src"
cat > "$tmp/exttool-deny/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/exttool-deny/qstar.lua" <<'EOF'
qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/ext.c")},
  command = qstar.cli {"qstar-extgen", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/ext.c"),
  },
}
EOF
if "$qstar" --file "$tmp/exttool-deny/qstar.lua" check //:app > "$tmp/exttool-deny.out" 2> "$tmp/exttool-deny.err"; then
	fail "unallowlisted PATH tool unexpectedly succeeded"
fi
contains "$tmp/exttool-deny.err" "generated action PATH tool 'qstar-extgen' is not allowed by profile path_tools"

mkdir -p "$tmp/tool-override/src" "$tmp/tool-override/tools"
cat > "$tmp/tool-override/tools/fake-objcopy.sh" <<'EOF'
#!/bin/sh
set -eu
out=$1
mkdir -p "$(dirname "$out")"
printf 'int override_value(void) { return 4; }\n' > "$out"
EOF
chmod +x "$tmp/tool-override/tools/fake-objcopy.sh"
cat > "$tmp/tool-override/src/main.c" <<'EOF'
int override_value(void);
int main(void) { return override_value() - 4; }
EOF
cat > "$tmp/tool-override/Cale.toml" <<'EOF'
profile = "tools"

[profile.tools]
tool_overrides = ["llvm-objcopy=tools/fake-objcopy.sh"]
EOF
cat > "$tmp/tool-override/qstar.lua" <<'EOF'
qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/override.c")},
  command = qstar.cli {"llvm-objcopy", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/override.c"),
  },
}
EOF
"$qstar" --file "$tmp/tool-override/qstar.lua" doctor > "$tmp/tool-override-doctor.out" 2> "$tmp/tool-override-doctor.err"
contains "$tmp/tool-override-doctor.out" "external-tool-policy path_tools=0 tool_overrides=1 allow_absolute=false"
contains "$tmp/tool-override-doctor.out" "external-tool-override name=llvm-objcopy value=tools/fake-objcopy.sh mode=package status=found"
"$qstar" --file "$tmp/tool-override/qstar.lua" dry-run //:app > "$tmp/tool-override-dry.out" 2> "$tmp/tool-override-dry.err"
contains "$tmp/tool-override-dry.out" "tool=llvm-objcopy tool_mode=override-package resolved_tool=tools/fake-objcopy.sh"
contains "$tmp/tool-override-dry.out" "argv=[tools/fake-objcopy.sh"
"$qstar" --file "$tmp/tool-override/qstar.lua" build //:app > "$tmp/tool-override-build.out" 2> "$tmp/tool-override-build.err"
contains "$tmp/tool-override-build.out" "tool=llvm-objcopy tool_mode=override-package resolved_tool=tools/fake-objcopy.sh"
contains "$tmp/tool-override/.qstar/logs/___generated_ext_generate_0.log" "argv[0]=tools/fake-objcopy.sh"
"$tmp/tool-override/.qstar/out/___app/app"

mkdir -p "$tmp/absolute-tool/src"
cat > "$tmp/absolute-tool/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/absolute-tool/qstar.lua" <<EOF
qstar.custom_target "generated_ext" {
  outputs = {qstar.output("generated/ext.c")},
  command = qstar.cli {"$tmp/exttool/bin/qstar-extgen", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/ext.c"),
  },
}
EOF
if "$qstar" --file "$tmp/absolute-tool/qstar.lua" check //:app > "$tmp/absolute-tool-deny.out" 2> "$tmp/absolute-tool-deny.err"; then
	fail "absolute external tool unexpectedly succeeded without profile capability"
fi
contains "$tmp/absolute-tool-deny.err" "requires allow_absolute_tools=true"
cat > "$tmp/absolute-tool/Cale.toml" <<'EOF'
profile = "abs"

[profile.abs]
allow_absolute_tools = true
EOF
"$qstar" --file "$tmp/absolute-tool/qstar.lua" dry-run //:app > "$tmp/absolute-tool-dry.out" 2> "$tmp/absolute-tool-dry.err"
contains "$tmp/absolute-tool-dry.out" "tool_mode=absolute"
contains "$tmp/absolute-tool-dry.out" "$tmp/exttool/bin/qstar-extgen"

mkdir -p "$tmp/longcmd/src"
cat > "$tmp/longcmd/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/longcmd/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {
        "include/very/long/path/segment/000",
        "include/very/long/path/segment/001",
        "include/very/long/path/segment/002",
        "include/very/long/path/segment/003",
        "include/very/long/path/segment/004",
        "include/very/long/path/segment/005",
        "include/very/long/path/segment/006",
        "include/very/long/path/segment/007",
        "include/very/long/path/segment/008",
        "include/very/long/path/segment/009",
        "include/very/long/path/segment/010",
        "include/very/long/path/segment/011",
        "include/very/long/path/segment/012",
        "include/very/long/path/segment/013",
        "include/very/long/path/segment/014",
        "include/very/long/path/segment/015",
        "include/very/long/path/segment/016",
        "include/very/long/path/segment/017",
        "include/very/long/path/segment/018",
        "include/very/long/path/segment/019",
      },
    },
  },
}
EOF
"$qstar" --file "$tmp/longcmd/qstar.lua" dry-run //:app > "$tmp/longcmd-dry.out" 2> "$tmp/longcmd-dry.err"
contains "$tmp/longcmd-dry.out" "response=skeleton"
contains "$tmp/longcmd-dry.out" "response_file=.qstar/rsp/"
contains "$tmp/longcmd-dry.out" "response_style=posix"
contains "$tmp/longcmd-dry.out" "response_digest="
"$qstar" --file "$tmp/longcmd/qstar.lua" build //:app > "$tmp/longcmd-build.out" 2> "$tmp/longcmd-build.err"
contains "$tmp/longcmd-build.out" "response_file id=//:app:compile:0"
contains "$tmp/longcmd-build.out" "style=posix"
contains "$tmp/longcmd-build.out" "digest="
test -d "$tmp/longcmd/.qstar/rsp" || fail "missing real response file dir"
contains "$tmp/longcmd-build.out" "status ok"

cat > "$tmp/longcmd/src/main.c" <<'EOF'
int main(void) { return ; }
EOF
if "$qstar" --file "$tmp/longcmd/qstar.lua" build //:app --explain-cache > "$tmp/longcmd-fail.out" 2> "$tmp/longcmd-fail.err"; then
	fail "long response-file failure unexpectedly succeeded"
fi
contains "$tmp/longcmd/.qstar/logs/last-failure.replay" "qstar failure replay v2"
contains "$tmp/longcmd/.qstar/logs/last-failure.replay" "argv_digest="
contains "$tmp/longcmd/.qstar/logs/last-failure.replay" "response_file path=.qstar/rsp/___app_compile_0.rsp style=posix digest="

mkdir -p "$tmp/rsppolicy/src"
cat > "$tmp/rsppolicy/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/rsppolicy/Cale.toml" <<'EOF'
profile = "norsp"

[profile.norsp]
toolchain = "clang"
target = "x86_64-unknown-none-elf"
response_files = "off"
EOF
cat > "$tmp/rsppolicy/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {
        "include/very/long/path/segment/000",
        "include/very/long/path/segment/001",
        "include/very/long/path/segment/002",
        "include/very/long/path/segment/003",
        "include/very/long/path/segment/004",
        "include/very/long/path/segment/005",
        "include/very/long/path/segment/006",
        "include/very/long/path/segment/007",
        "include/very/long/path/segment/008",
        "include/very/long/path/segment/009",
        "include/very/long/path/segment/010",
        "include/very/long/path/segment/011",
      },
    },
  },
}
EOF
"$qstar" --file "$tmp/rsppolicy/qstar.lua" dry-run //:app > "$tmp/rsppolicy-dry.out" 2> "$tmp/rsppolicy-dry.err"
contains "$tmp/rsppolicy-dry.out" "response=unsupported response_capability=off"
contains "$tmp/rsppolicy-dry.out" "response_files=off response_style=posix"

mkdir -p "$tmp/windows/src"
cat > "$tmp/windows/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/windows/Cale.toml" <<'EOF'
profile = "msvc"

[profile.msvc]
toolchain = "clang"
target = "x86_64-pc-windows-msvc"
cc = "clang-cl"
cxx = "clang-cl"
linker = "clang-cl"
response_style = "msvc"
EOF
cat > "$tmp/windows/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      include_dirs = {
        "win include dir",
        "sdk\\include\\tail\\",
        "include/very/long/path/segment/000",
        "include/very/long/path/segment/001",
        "include/very/long/path/segment/002",
        "include/very/long/path/segment/003",
        "include/very/long/path/segment/004",
        "include/very/long/path/segment/005",
        "include/very/long/path/segment/006",
        "include/very/long/path/segment/007",
        "include/very/long/path/segment/008",
        "include/very/long/path/segment/009",
      },
    },
  },
  lib_dirs = {"win lib"},
  libs = {"user32"},
}
EOF
"$qstar" --file "$tmp/windows/qstar.lua" dry-run //:app > "$tmp/windows-dry.out" 2> "$tmp/windows-dry.err"
contains "$tmp/windows-dry.out" "response_style=msvc"
contains "$tmp/windows-dry.out" "response_digest="
contains "$tmp/windows-dry.out" "/link"
contains "$tmp/windows-dry.out" "/LIBPATH:win lib"
contains "$tmp/windows-dry.out" "user32.lib"

mkdir -p "$tmp/artifact/tools" "$tmp/artifact/fixtures"
cat > "$tmp/artifact/tools/fake-objcopy.sh" <<'EOF'
#!/bin/sh
set -eu
fmt=
in=
out=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-O)
			shift
			fmt=$1
			;;
		*)
			if [ -z "${in:-}" ]; then
				in=$1
			else
				out=$1
			fi
			;;
	esac
	shift
done
test "$fmt" = binary
test -n "$in"
test -n "$out"
mkdir -p "$(dirname "$out")"
cp "$in" "$out"
EOF
chmod +x "$tmp/artifact/tools/fake-objcopy.sh"
cat > "$tmp/artifact/fixtures/kernel.elf" <<'EOF'
ELF-STUB
payload
EOF
cat > "$tmp/artifact/Cale.toml" <<'EOF'
profile = "boot"

[profile.boot]
tool_overrides = ["llvm-objcopy=tools/fake-objcopy.sh"]
EOF
cat > "$tmp/artifact/qstar.lua" <<'EOF'
qstar.custom_target "kernel_img" {
  inputs = {"fixtures/kernel.elf"},
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
    }),
  },
  command = qstar.cli {
    "llvm-objcopy",
    "-O",
    "binary",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.run_target "smoke" {
  command = qstar.cli {"tools/fake-objcopy.sh", "-O", "binary", qstar.target_file("//:kernel_img"), "generated/copy.img"},
}
EOF
"$qstar" --file "$tmp/artifact/qstar.lua" check //:kernel_img > "$tmp/artifact-check.out" 2> "$tmp/artifact-check.err"
contains "$tmp/artifact-check.out" "generated-action-count 1"
"$qstar" --file "$tmp/artifact/qstar.lua" explain //:kernel_img > "$tmp/artifact-explain.out" 2> "$tmp/artifact-explain.err"
contains "$tmp/artifact-explain.out" "plan_generated_action //:kernel_img"
contains "$tmp/artifact-explain.out" "generated_artifact output=generated/kernel8.img group=images format=raw-binary address=0x80000 layout=rpi5-kernel8"
contains "$tmp/artifact-explain.out" "identity=generated/kernel8.img|group=images|format=raw-binary|address=0x80000|layout=rpi5-kernel8"
"$qstar" --file "$tmp/artifact/qstar.lua" dry-run //:kernel_img > "$tmp/artifact-dry.out" 2> "$tmp/artifact-dry.err"
contains "$tmp/artifact-dry.out" "dry_run_generated_action //:kernel_img"
contains "$tmp/artifact-dry.out" "tool=llvm-objcopy tool_mode=override-package resolved_tool=tools/fake-objcopy.sh"
contains "$tmp/artifact-dry.out" "argv=[tools/fake-objcopy.sh, -O, binary, fixtures/kernel.elf, generated/kernel8.img]"
"$qstar" --file "$tmp/artifact/qstar.lua" build //:kernel_img > "$tmp/artifact-build.out" 2> "$tmp/artifact-build.err"
contains "$tmp/artifact-build.out" "build_generated_action //:kernel_img"
contains "$tmp/artifact-build.out" "output_identity=[generated/kernel8.img|group=images|format=raw-binary|address=0x80000|layout=rpi5-kernel8]"
contains "$tmp/artifact-build.out" "status ok"
test -f "$tmp/artifact/generated/kernel8.img" || fail "missing generated raw image artifact"
cmp "$tmp/artifact/fixtures/kernel.elf" "$tmp/artifact/generated/kernel8.img" >/dev/null || fail "raw image artifact content drifted"
contains "$tmp/artifact/.qstar/state/graph.json" "\"output_artifacts\""
contains "$tmp/artifact/.qstar/state/graph.json" "\"format\":\"raw-binary\""
contains "$tmp/artifact/.qstar/state/actions.json" "\"output\":\"generated/kernel8.img\""
contains "$tmp/artifact/.qstar/logs/___kernel_img_generate_0.log" "argv[0]=tools/fake-objcopy.sh"
"$qstar" --file "$tmp/artifact/qstar.lua" build //:smoke > "$tmp/artifact-smoke.out" 2> "$tmp/artifact-smoke.err"
contains "$tmp/artifact-smoke.out" "run_target label=//:smoke"
test -f "$tmp/artifact/generated/copy.img" || fail "target_file generated artifact path was not consumed"
"$qstar" --file "$tmp/artifact/qstar.lua" list-targets --format json > "$tmp/artifact-targets-json.out" 2> "$tmp/artifact-targets-json.err"
contains "$tmp/artifact-targets-json.out" "\"output_artifacts\""
contains "$tmp/artifact-targets-json.out" "\"group\":\"images\""
contains "$tmp/artifact-targets-json.out" "\"format\":\"raw-binary\""

mkdir -p "$tmp/artifact-collision/tools" "$tmp/artifact-collision/fixtures"
cp "$tmp/artifact/tools/fake-objcopy.sh" "$tmp/artifact-collision/tools/fake-objcopy.sh"
cp "$tmp/artifact/fixtures/kernel.elf" "$tmp/artifact-collision/fixtures/kernel.elf"
cat > "$tmp/artifact-collision/Cale.toml" <<'EOF'
profile = "boot"

[profile.boot]
tool_overrides = ["llvm-objcopy=tools/fake-objcopy.sh"]
EOF
cat > "$tmp/artifact-collision/qstar.lua" <<'EOF'
qstar.custom_target "one" {
  inputs = {"fixtures/kernel.elf"},
  outputs = {qstar.output("generated/one.img", {format = "raw-binary", address = "0x80000", layout = "rpi5-kernel8"})},
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}

qstar.custom_target "two" {
  inputs = {"fixtures/kernel.elf"},
  outputs = {qstar.output("generated/two.img", {group = "images", format = "raw-binary", address = "0x80000", layout = "rpi5-kernel8"})},
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/artifact-collision/qstar.lua" check > "$tmp/artifact-collision.out" 2> "$tmp/artifact-collision.err"; then
	fail "duplicate artifact identity unexpectedly succeeded"
fi
contains "$tmp/artifact-collision.err" "generated artifact identity group=images format=raw-binary address=0x80000 layout=rpi5-kernel8 has multiple outputs"

mkdir -p "$tmp/artifact-badmeta"
cat > "$tmp/artifact-badmeta/qstar.lua" <<'EOF'
qstar.custom_target "bad" {
  outputs = {qstar.output("generated/bad.img", {unknown = "x"})},
  command = qstar.cli {"tools/fake.sh", qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/artifact-badmeta/qstar.lua" check > "$tmp/artifact-badmeta.out" 2> "$tmp/artifact-badmeta.err"; then
	fail "unknown artifact metadata unexpectedly succeeded"
fi
contains "$tmp/artifact-badmeta.err" "unknown qstar.output metadata field 'unknown'"
cat > "$tmp/artifact-badmeta/qstar.lua" <<'EOF'
qstar.custom_target "bad" {
  outputs = {qstar.output("generated/bad.img", {format = true})},
  command = qstar.cli {"tools/fake.sh", qstar.output(0)},
}
EOF
if "$qstar" --file "$tmp/artifact-badmeta/qstar.lua" check > "$tmp/artifact-badmeta-type.out" 2> "$tmp/artifact-badmeta-type.err"; then
	fail "non-string artifact metadata unexpectedly succeeded"
fi
contains "$tmp/artifact-badmeta-type.err" "qstar.output metadata field 'format' must be a string"

mkdir -p "$tmp/blob-embed/tools" "$tmp/blob-embed/fixtures" "$tmp/blob-embed/src"
cat > "$tmp/blob-embed/fixtures/payload.elf" <<'EOF'
ELF-FIXTURE-V1
payload
EOF
cat > "$tmp/blob-embed/tools/embed-asm.sh" <<'EOF'
#!/bin/sh
set -eu
input=$1
output=$2
bytes=$(wc -c < "$input" | tr -d ' ')
mkdir -p "$(dirname "$output")"
{
	printf '/* qstar binary blob assembly placeholder */\n'
	printf '/* input=%s bytes=%s */\n' "$input" "$bytes"
} > "$output"
EOF
chmod +x "$tmp/blob-embed/tools/embed-asm.sh"
cat > "$tmp/blob-embed/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/blob-embed/qstar.lua" <<'EOF'
qstar.custom_target "embed_asm" {
  inputs = {"fixtures/payload.elf"},
  outputs = {
    qstar.output("generated/blob.S", {
      group = "objects",
      format = "assembly",
      layout = "rpi5-elf-fixture-embed",
    }),
  },
  command = qstar.cli {"tools/embed-asm.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/blob.S"),
  },
}
EOF
"$qstar" --file "$tmp/blob-embed/qstar.lua" explain //:app > "$tmp/blob-embed-explain.out" 2> "$tmp/blob-embed-explain.err"
contains "$tmp/blob-embed-explain.out" "generated_edge source=generated/blob.S generator=//:embed_asm"
contains "$tmp/blob-embed-explain.out" "generated_artifact output=generated/blob.S group=objects format=assembly"
"$qstar" --file "$tmp/blob-embed/qstar.lua" build //:app --explain-cache > "$tmp/blob-embed-first.out" 2> "$tmp/blob-embed-first.err"
contains "$tmp/blob-embed-first.out" "build_action id=//:embed_asm:generate:0 status=run"
contains "$tmp/blob-embed-first.out" "build_action id=//:app:compile:1 status=run"
contains "$tmp/blob-embed-first.out" "status ok"
test -f "$tmp/blob-embed/generated/blob.S" || fail "missing generated blob assembly"
"$qstar" --file "$tmp/blob-embed/qstar.lua" build //:app --explain-cache > "$tmp/blob-embed-second.out" 2> "$tmp/blob-embed-second.err"
contains "$tmp/blob-embed-second.out" "build_action id=//:embed_asm:generate:0 status=skip"
contains "$tmp/blob-embed-second.out" "build_action id=//:app:compile:1 status=skip"
printf 'payload-v2\n' >> "$tmp/blob-embed/fixtures/payload.elf"
"$qstar" --file "$tmp/blob-embed/qstar.lua" build //:app --explain-cache > "$tmp/blob-embed-third.out" 2> "$tmp/blob-embed-third.err"
contains "$tmp/blob-embed-third.out" "cache_miss id=//:embed_asm:generate:0 reason=input-changed"
contains "$tmp/blob-embed-third.out" "cache_miss id=//:app:compile:1 reason=depfile-changed"

mkdir -p "$tmp/blob-object/tools" "$tmp/blob-object/fixtures" "$tmp/blob-object/src"
cat > "$tmp/blob-object/fixtures/payload.elf" <<'EOF'
ELF-FIXTURE-OBJECT-V1
payload
EOF
cat > "$tmp/blob-object/tools/embed-object.sh" <<'EOF'
#!/bin/sh
set -eu
input=$1
output=$2
bytes=$(wc -c < "$input" | tr -d ' ')
mkdir -p "$(dirname "$output")"
tmp="${output}.c"
cat > "$tmp" <<EOF_C
int qstar_embedded_blob_len = $bytes;
EOF_C
${CC:-cc} -c "$tmp" -o "$output"
EOF
chmod +x "$tmp/blob-object/tools/embed-object.sh"
cat > "$tmp/blob-object/src/main.c" <<'EOF'
extern int qstar_embedded_blob_len;
int main(void) { return qstar_embedded_blob_len > 0 ? 0 : 1; }
EOF
cat > "$tmp/blob-object/qstar.lua" <<'EOF'
qstar.custom_target "embed_object" {
  inputs = {"fixtures/payload.elf"},
  outputs = {
    qstar.output("generated/blob.o", {
      format = "object",
      layout = "rpi5-elf-fixture-embed",
    }),
  },
  command = qstar.cli {"tools/embed-object.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "objapp" {
  sources = {
    "src/main.c",
    qstar.output("generated/blob.o"),
  },
}
EOF
"$qstar" --file "$tmp/blob-object/qstar.lua" explain //:objapp > "$tmp/blob-object-explain.out" 2> "$tmp/blob-object-explain.err"
contains "$tmp/blob-object-explain.out" "generated_artifact output=generated/blob.o group=objects format=object"
contains "$tmp/blob-object-explain.out" "action link-input source=generated/blob.o language=object output=generated/blob.o"
"$qstar" --file "$tmp/blob-object/qstar.lua" dry-run //:objapp > "$tmp/blob-object-dry.out" 2> "$tmp/blob-object-dry.err"
contains "$tmp/blob-object-dry.out" "dry_run_step id=//:objapp:link-input:1 owner=//:objapp kind=link-input language=object"
"$qstar" --file "$tmp/blob-object/qstar.lua" build //:objapp --explain-cache > "$tmp/blob-object-first.out" 2> "$tmp/blob-object-first.err"
contains "$tmp/blob-object-first.out" "build_action id=//:embed_object:generate:0 status=run"
contains "$tmp/blob-object-first.out" "build_action id=//:objapp:link:0 status=run"
contains "$tmp/blob-object-first.out" "status ok"
"$tmp/blob-object/.qstar/out/___objapp/objapp"
"$qstar" --file "$tmp/blob-object/qstar.lua" build //:objapp --explain-cache > "$tmp/blob-object-second.out" 2> "$tmp/blob-object-second.err"
contains "$tmp/blob-object-second.out" "build_action id=//:embed_object:generate:0 status=skip"
contains "$tmp/blob-object-second.out" "build_action id=//:objapp:link:0 status=skip"
printf 'payload-v2\n' >> "$tmp/blob-object/fixtures/payload.elf"
"$qstar" --file "$tmp/blob-object/qstar.lua" build //:objapp --explain-cache > "$tmp/blob-object-third.out" 2> "$tmp/blob-object-third.err"
contains "$tmp/blob-object-third.out" "cache_miss id=//:embed_object:generate:0 reason=input-changed"
contains "$tmp/blob-object-third.out" "cache_miss id=//:objapp:link:0 reason=input-changed"

mkdir -p "$tmp/uefi/src" "$tmp/uefi/tools"
cat > "$tmp/uefi/tools/fake-clang.sh" <<'EOF'
#!/bin/sh
set -eu
out=
dep=
src=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-o)
			shift
			out=$1
			;;
		-MF)
			shift
			dep=$1
			;;
		-c)
			shift
			src=$1
			;;
	esac
	shift || break
done
test -n "$out"
mkdir -p "$(dirname "$out")"
printf "pe-coff-object\n" > "$out"
if [ -n "$dep" ]; then
	mkdir -p "$(dirname "$dep")"
	printf "%s: %s\n" "$out" "$src" > "$dep"
fi
EOF
cat > "$tmp/uefi/tools/fake-lld-link.sh" <<'EOF'
#!/bin/sh
set -eu
out=
subsystem=0
entry=0
nodefault=0
for arg in "$@"; do
	case "$arg" in
		/out:*)
			out=${arg#/out:}
			;;
		/subsystem:efi_application)
			subsystem=1
			;;
		/entry:efi_main)
			entry=1
			;;
		/nodefaultlib)
			nodefault=1
			;;
	esac
done
test -n "$out"
test "$subsystem" = 1
test "$entry" = 1
test "$nodefault" = 1
mkdir -p "$(dirname "$out")"
printf "MZ\nUEFI\n" > "$out"
EOF
chmod +x "$tmp/uefi/tools/fake-clang.sh" "$tmp/uefi/tools/fake-lld-link.sh"
cat > "$tmp/uefi/src/efi_main.c" <<'EOF'
int efi_main(void *image, void *system_table) {
	(void)image;
	(void)system_table;
	return 0;
}
EOF
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.executable "boot" {
  sources = {"src/efi_main.c"},
  lang = {
    c = {
      compile_options = {"-ffreestanding"},
    },
  },
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}
EOF
cat > "$tmp/uefi/Cale.toml" <<'EOF'
profile = "uefi-x64"

[profile.uefi-x64]
toolchain = "clang"
target = "x86_64-pc-windows-msvc"
cc = "tools/fake-clang.sh"
linker = "tools/fake-lld-link.sh"
response_style = "msvc"
artifact_names = ["//:boot=BOOTX64.EFI"]
EOF
"$qstar" --file "$tmp/uefi/qstar.lua" dry-run //:boot > "$tmp/uefi-x64-dry.out" 2> "$tmp/uefi-x64-dry.err"
contains "$tmp/uefi-x64-dry.out" "response_style=msvc"
contains "$tmp/uefi-x64-dry.out" "/out:.qstar/out/___boot/BOOTX64.EFI"
contains "$tmp/uefi-x64-dry.out" "/subsystem:efi_application"
contains "$tmp/uefi-x64-dry.out" "/entry:efi_main"
contains "$tmp/uefi-x64-dry.out" "/nodefaultlib"
"$qstar" --file "$tmp/uefi/qstar.lua" build //:boot > "$tmp/uefi-x64-build.out" 2> "$tmp/uefi-x64-build.err"
contains "$tmp/uefi-x64-build.out" "status ok"
test -f "$tmp/uefi/.qstar/out/___boot/BOOTX64.EFI" || fail "missing UEFI x64 artifact"
contains "$tmp/uefi/.qstar/logs/___boot_link_0.log" "argv[0]=tools/fake-lld-link.sh"
contains "$tmp/uefi/.qstar/logs/___boot_link_0.log" "argv[1]=/out:.qstar/out/___boot/BOOTX64.EFI"
contains "$tmp/uefi/.qstar/logs/___boot_link_0.log" "argv[2]=/subsystem:efi_application"
contains "$tmp/uefi/.qstar/state/graph.json" "\"artifact_name\":\"\""
cat > "$tmp/uefi/Cale.toml" <<'EOF'
profile = "uefi-aa64"

[profile.uefi-aa64]
toolchain = "clang"
target = "aarch64-pc-windows-msvc"
cc = "tools/fake-clang.sh"
linker = "tools/fake-lld-link.sh"
response_style = "msvc"
artifact_names = ["//:boot=BOOTAA64.EFI"]
EOF
"$qstar" --file "$tmp/uefi/qstar.lua" dry-run //:boot > "$tmp/uefi-aa64-dry.out" 2> "$tmp/uefi-aa64-dry.err"
contains "$tmp/uefi-aa64-dry.out" "/out:.qstar/out/___boot/BOOTAA64.EFI"
"$qstar" --file "$tmp/uefi/qstar.lua" build //:boot > "$tmp/uefi-aa64-build.out" 2> "$tmp/uefi-aa64-build.err"
contains "$tmp/uefi-aa64-build.out" "status ok"
test -f "$tmp/uefi/.qstar/out/___boot/BOOTAA64.EFI" || fail "missing UEFI AArch64 artifact"
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.executable "boot" {
  artifact_name = "BOOTLOCAL.EFI",
  sources = {"src/efi_main.c"},
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}
EOF
"$qstar" --file "$tmp/uefi/qstar.lua" dry-run //:boot > "$tmp/uefi-local-dry.out" 2> "$tmp/uefi-local-dry.err"
contains "$tmp/uefi-local-dry.out" "/out:.qstar/out/___boot/BOOTLOCAL.EFI"
"$qstar" --file "$tmp/uefi/qstar.lua" list-targets --format json > "$tmp/uefi-targets-json.out" 2> "$tmp/uefi-targets-json.err"
contains "$tmp/uefi-targets-json.out" "\"artifact_name\":\"BOOTLOCAL.EFI\""
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.executable "boot" {
  artifact_name = "EFI/BOOT/BOOTX64.EFI",
  sources = {"src/efi_main.c"},
}
EOF
if "$qstar" --file "$tmp/uefi/qstar.lua" check //:boot > "$tmp/uefi-bad-target-name.out" 2> "$tmp/uefi-bad-target-name.err"; then
	fail "path-like artifact_name unexpectedly succeeded"
fi
contains "$tmp/uefi-bad-target-name.err" "artifact_name 'EFI/BOOT/BOOTX64.EFI' must be a filename, not a path"
cat > "$tmp/uefi/qstar.lua" <<'EOF'
qstar.executable "boot" {
  sources = {"src/efi_main.c"},
}
EOF
cat > "$tmp/uefi/Cale.toml" <<'EOF'
profile = "bad"

[profile.bad]
toolchain = "clang"
target = "x86_64-pc-windows-msvc"
cc = "tools/fake-clang.sh"
linker = "tools/fake-lld-link.sh"
artifact_names = ["//:boot=EFI/BOOT/BOOTX64.EFI"]
EOF
if "$qstar" --file "$tmp/uefi/qstar.lua" check //:boot > "$tmp/uefi-bad-profile-name.out" 2> "$tmp/uefi-bad-profile-name.err"; then
	fail "path-like profile artifact_names unexpectedly succeeded"
fi
contains "$tmp/uefi-bad-profile-name.err" "profile artifact_names entry '//:boot=EFI/BOOT/BOOTX64.EFI' must be LABEL=FILENAME"

mkdir -p "$tmp/stagepkg/src" "$tmp/stagepkg/tools" "$tmp/stagepkg/fixtures" "$tmp/stagepkg/boot"
cat > "$tmp/stagepkg/tools/fake-clang.sh" <<'EOF'
#!/bin/sh
set -eu
out=
dep=
src=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-o)
			shift
			out=$1
			;;
		-MF)
			shift
			dep=$1
			;;
		-c)
			shift
			src=$1
			;;
	esac
	shift || break
done
test -n "$out"
mkdir -p "$(dirname "$out")"
printf "boot-object\n" > "$out"
if [ -n "$dep" ]; then
	mkdir -p "$(dirname "$dep")"
	printf "%s: %s\n" "$out" "$src" > "$dep"
fi
EOF
cat > "$tmp/stagepkg/tools/fake-link.sh" <<'EOF'
#!/bin/sh
set -eu
out=
for arg in "$@"; do
	case "$arg" in
		-o)
			shift
			out=$1
			;;
		/out:*)
			out=${arg#/out:}
			;;
	esac
	shift || break
done
test -n "$out"
mkdir -p "$(dirname "$out")"
printf "MZ\nBOOT\n" > "$out"
EOF
cat > "$tmp/stagepkg/tools/fake-objcopy.sh" <<'EOF'
#!/bin/sh
set -eu
format=
input=
output=
while [ "$#" -gt 0 ]; do
	case "$1" in
		-O)
			shift
			format=$1
			;;
		*)
			if [ -z "$input" ]; then
				input=$1
			else
				output=$1
			fi
			;;
	esac
	shift || break
done
test "$format" = binary
test -n "$input"
test -n "$output"
mkdir -p "$(dirname "$output")"
cp "$input" "$output"
EOF
chmod +x "$tmp/stagepkg/tools/fake-clang.sh" "$tmp/stagepkg/tools/fake-link.sh" "$tmp/stagepkg/tools/fake-objcopy.sh"
cat > "$tmp/stagepkg/src/efi_main.c" <<'EOF'
int efi_main(void *image, void *system_table) {
	(void)image;
	(void)system_table;
	return 0;
}
EOF
cat > "$tmp/stagepkg/fixtures/kernel.elf" <<'EOF'
ELF-RPI
payload
EOF
cat > "$tmp/stagepkg/boot/config.txt" <<'EOF'
kernel=kernel8.img
EOF
cat > "$tmp/stagepkg/boot/payload.bin" <<'EOF'
payload
EOF
cat > "$tmp/stagepkg/Cale.toml" <<'EOF'
profile = "boot"

[profile.boot]
toolchain = "clang"
target = "x86_64-pc-windows-msvc"
cc = "tools/fake-clang.sh"
linker = "tools/fake-link.sh"
response_style = "msvc"
tool_overrides = ["llvm-objcopy=tools/fake-objcopy.sh"]
artifact_names = ["//:boot=BOOTX64.EFI"]
EOF
cat > "$tmp/stagepkg/qstar.lua" <<'EOF'
qstar.executable "boot" {
  sources = {"src/efi_main.c"},
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}

qstar.custom_target "kernel_img" {
  inputs = {"fixtures/kernel.elf"},
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
    }),
  },
  command = qstar.cli {
    "llvm-objcopy",
    "-O",
    "binary",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.stage "esp" {
  root = "stage/esp",
  files = {
    qstar.stage_file(qstar.target_file("//:boot"), "EFI/BOOT/BOOTX64.EFI"),
  },
}

qstar.stage "rpi" {
  root = "stage/rpi",
  files = {
    qstar.stage_file("boot/config.txt", "config.txt"),
    qstar.stage_file(qstar.target_file("//:kernel_img"), "kernel8.img"),
    qstar.stage_file("boot/payload.bin", "payload.bin"),
  },
}
EOF
"$qstar" --file "$tmp/stagepkg/qstar.lua" check //:boot > "$tmp/stagepkg-check.out" 2> "$tmp/stagepkg-check.err"
contains "$tmp/stagepkg-check.out" "stage-count 2"
"$qstar" --file "$tmp/stagepkg/qstar.lua" list-targets --format json > "$tmp/stagepkg-targets-json.out" 2> "$tmp/stagepkg-targets-json.err"
contains "$tmp/stagepkg-targets-json.out" "\"stage_count\":2"
contains "$tmp/stagepkg-targets-json.out" "\"label\":\"//:esp\""
contains "$tmp/stagepkg-targets-json.out" "\"root\":\"stage/esp\""
"$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:esp --dry-run > "$tmp/stagepkg-esp-dry.out" 2> "$tmp/stagepkg-esp-dry.err"
contains "$tmp/stagepkg-esp-dry.out" "qstar stage v1"
contains "$tmp/stagepkg-esp-dry.out" "mode dry-run"
contains "$tmp/stagepkg-esp-dry.out" "stage_file src=.qstar/out/___boot/BOOTX64.EFI dst=stage/esp/EFI/BOOT/BOOTX64.EFI mode=dry-run"
contains "$tmp/stagepkg-esp-dry.out" "stage_diff dst=stage/esp/EFI/BOOT/BOOTX64.EFI action=would-create"
contains "$tmp/stagepkg/.qstar/stage/___esp/manifest.json" "\"schema\":\"qstar-stage-manifest-v1\""
contains "$tmp/stagepkg/.qstar/stage/___esp/manifest.json" "\"mode\":\"dry-run\""
if [ -f "$tmp/stagepkg/stage/esp/EFI/BOOT/BOOTX64.EFI" ]; then
	fail "stage dry-run unexpectedly copied ESP artifact"
fi
"$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:esp > "$tmp/stagepkg-esp-stage.out" 2> "$tmp/stagepkg-esp-stage.err"
contains "$tmp/stagepkg-esp-stage.out" "qstar build v2"
contains "$tmp/stagepkg-esp-stage.out" "stage_diff dst=stage/esp/EFI/BOOT/BOOTX64.EFI action=would-create"
contains "$tmp/stagepkg-esp-stage.out" "status ok"
test -f "$tmp/stagepkg/stage/esp/EFI/BOOT/BOOTX64.EFI" || fail "missing staged ESP BOOTX64.EFI"
contains "$tmp/stagepkg/.qstar/stage/___esp/manifest.json" "\"mode\":\"copy\""
"$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:esp --dry-run > "$tmp/stagepkg-esp-dry2.out" 2> "$tmp/stagepkg-esp-dry2.err"
contains "$tmp/stagepkg-esp-dry2.out" "stage_diff dst=stage/esp/EFI/BOOT/BOOTX64.EFI action=unchanged"
"$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:rpi > "$tmp/stagepkg-rpi-stage.out" 2> "$tmp/stagepkg-rpi-stage.err"
contains "$tmp/stagepkg-rpi-stage.out" "stage_file src=boot/config.txt dst=stage/rpi/config.txt mode=copy"
contains "$tmp/stagepkg-rpi-stage.out" "stage_file src=generated/kernel8.img dst=stage/rpi/kernel8.img mode=copy"
contains "$tmp/stagepkg-rpi-stage.out" "stage_file src=boot/payload.bin dst=stage/rpi/payload.bin mode=copy"
test -f "$tmp/stagepkg/stage/rpi/config.txt" || fail "missing staged RPi config.txt"
test -f "$tmp/stagepkg/stage/rpi/kernel8.img" || fail "missing staged RPi kernel8.img"
test -f "$tmp/stagepkg/stage/rpi/payload.bin" || fail "missing staged RPi payload.bin"
cmp "$tmp/stagepkg/fixtures/kernel.elf" "$tmp/stagepkg/stage/rpi/kernel8.img" >/dev/null || fail "staged RPi image content drifted"
contains "$tmp/stagepkg/.qstar/stage/___rpi/manifest.json" "\"label\":\"//:rpi\""
contains "$tmp/stagepkg/.qstar/stage/___rpi/manifest.json" "\"dst\":\"stage/rpi/kernel8.img\""
"$qstar" --file "$tmp/stagepkg/qstar.lua" stage //:esp --root stage/custom-esp --dry-run > "$tmp/stagepkg-esp-root.out" 2> "$tmp/stagepkg-esp-root.err"
contains "$tmp/stagepkg-esp-root.out" "stage-root stage/custom-esp"
contains "$tmp/stagepkg-esp-root.out" "dst=stage/custom-esp/EFI/BOOT/BOOTX64.EFI"

mkdir -p "$tmp/stage-bad/src"
cat > "$tmp/stage-bad/src/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
}

qstar.stage "bad" {
  root = "../stage",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "app.bin"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-root.out" 2> "$tmp/stage-bad-root.err"; then
	fail "stage root escape unexpectedly succeeded"
fi
contains "$tmp/stage-bad-root.err" "stage root '../stage' in '//:bad' must be package-relative"
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
}

qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "../app.bin"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-dst.out" 2> "$tmp/stage-bad-dst.err"; then
	fail "stage destination escape unexpectedly succeeded"
fi
contains "$tmp/stage-bad-dst.err" "stage destination '../app.bin' in '//:bad' must be package-relative"
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.executable "app" {
  sources = {"src/main.c"},
}

qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "app.bin"),
    qstar.stage_file("src/main.c", "app.bin"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-dup.out" 2> "$tmp/stage-bad-dup.err"; then
	fail "duplicate stage destination unexpectedly succeeded"
fi
contains "$tmp/stage-bad-dup.err" "stage destination 'app.bin' in '//:bad' is duplicated"
cat > "$tmp/stage-bad/qstar.lua" <<'EOF'
qstar.stage "bad" {
  root = "stage/bad",
  files = {
    qstar.stage_file(qstar.target_file("//:missing"), "missing.bin"),
  },
}
EOF
if "$qstar" --file "$tmp/stage-bad/qstar.lua" check > "$tmp/stage-bad-missing.out" 2> "$tmp/stage-bad-missing.err"; then
	fail "unknown stage target_file unexpectedly succeeded"
fi
contains "$tmp/stage-bad-missing.err" "stage source target '//:missing' in '//:bad' is unknown"

echo "qstar-smoke: passed"
