#!/bin/sh
set -eu

repo=${QSTAR_WIKI_REPO:-https://github.com/deeyed/qstar.wiki.git}
src=${QSTAR_WIKI_SRC:-wiki}
work=${QSTAR_WIKI_WORKTREE:-}

if [ ! -d "$src" ]; then
	printf 'qstar-wiki-sync: source wiki directory not found: %s\n' "$src" >&2
	exit 1
fi

cleanup=
if [ -z "$work" ]; then
	work=$(mktemp -d "${TMPDIR:-/tmp}/qstar-wiki.XXXXXX")
	cleanup=$work
fi

if [ -n "$cleanup" ]; then
	trap 'rm -rf "$cleanup"' EXIT HUP INT TERM
fi

if git clone "$repo" "$work"; then
	:
else
	printf 'qstar-wiki-sync: wiki remote clone failed; initializing a new wiki checkout\n' >&2
	mkdir -p "$work"
	git init "$work"
	git -C "$work" remote add origin "$repo"
fi

find "$work" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -R "$src"/. "$work"/
cp "$src/README.md" "$work/Home.md"

cat > "$work/_Sidebar.md" <<'EOF'
# QStar

- [Home](Home)
- [Getting Started](getting-started)
- [Installation](installation)
- [AI Index](AI_INDEX)

## Concepts

- [Workspace, Project, Package](concepts/workspace-project-package)
- [Labels And Fragments](concepts/labels-and-fragments)
- [Targets And Actions](concepts/targets-and-actions)
- [Language Namespaces](concepts/language-namespaces)

## Reference

- [QStar Lua](reference/qstar-lua)
- [Imports And Modules](reference/modules)
- [Reusable Configs](reference/configs)
- [Target Rules](reference/target-rules)
- [Profiles](reference/profiles)
- [Diagnostics](reference/diagnostics)
- [Progress Output](reference/progress-output)
EOF

(
	cd "$work"
	git add .
	if git diff --cached --quiet; then
		printf 'qstar-wiki-sync: no changes\n'
	else
		git -c user.name="${QSTAR_WIKI_GIT_NAME:-QStar Release Bot}" \
			-c user.email="${QSTAR_WIKI_GIT_EMAIL:-qstar-release@users.noreply.github.com}" \
			commit -m "Sync QStar wiki"
		git push origin HEAD:master
	fi
)
