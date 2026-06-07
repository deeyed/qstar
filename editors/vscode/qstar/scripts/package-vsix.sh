#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(node -e 'const fs=require("fs"); const p=JSON.parse(fs.readFileSync(process.argv[1],"utf8")); process.stdout.write(p.version);' "$root/package.json")
out="$root/dist/qstar-vscode-$version.vsix"

if command -v vsce >/dev/null 2>&1; then
	vsce_bin=vsce
elif command -v npx >/dev/null 2>&1 && npx --no-install @vscode/vsce --version >/dev/null 2>&1; then
	vsce_bin="npx --no-install @vscode/vsce"
else
	echo "qstar-vscode: missing VSCode package tool; install @vscode/vsce outside the repo" >&2
	echo "qstar-vscode: this script never vendors node_modules or commits .vsix artifacts" >&2
	exit 127
fi

cd "$root"
rm -f dist/*.vsix
node scripts/check-package.js "$root"
mkdir -p dist
set -- package --out "$out"
echo "qstar-vscode: packaging $out"
# shellcheck disable=SC2086
$vsce_bin "$@"
