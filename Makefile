.POSIX:

CC ?= cc
BUILD_DIR ?= build
BIN_DIR ?= $(BUILD_DIR)/bin
PREFIX ?= /usr/local
DOC_DIR ?= $(PREFIX)/share/doc/qstar
MAN_DIR ?= $(PREFIX)/share/man
PROVIDER_DIR ?= $(PREFIX)/share/qstar/languages
QSTAR_BUILD = $(BUILD_DIR)/obj
LUA_DIR = vendor/lua
CFLAGS ?= -g -O0 -pipe
LDLIBS ?= -lm
QSTAR_CFLAGS = -std=c99 -Wall -Wextra -Wpedantic $(CFLAGS) -Iinclude -I$(LUA_DIR)
LUA_CFLAGS = -std=c99 -O2 -I$(LUA_DIR)

QSTAR_SRCS = \
	src/daemon.c \
	src/executor.c \
	src/fmt.c \
	src/graph.c \
	src/header.c \
	src/init.c \
	src/label.c \
	src/lint.c \
	src/lsp.c \
	src/lua_runtime.c \
	src/ninja.c \
	src/platform_process.c \
	src/plan.c \
	src/tool_context.c \
	src/tool_resolution.c \
	src/external_tool_policy.c \
	src/rule.c \
	src/source.c \
	src/stella_cache.c \
	src/test_suite.c \
	src/main.c

LUA_SRCS = \
	$(LUA_DIR)/lapi.c \
	$(LUA_DIR)/lauxlib.c \
	$(LUA_DIR)/lbaselib.c \
	$(LUA_DIR)/lcode.c \
	$(LUA_DIR)/lctype.c \
	$(LUA_DIR)/ldebug.c \
	$(LUA_DIR)/ldo.c \
	$(LUA_DIR)/ldump.c \
	$(LUA_DIR)/lfunc.c \
	$(LUA_DIR)/lgc.c \
	$(LUA_DIR)/llex.c \
	$(LUA_DIR)/lmem.c \
	$(LUA_DIR)/lobject.c \
	$(LUA_DIR)/lopcodes.c \
	$(LUA_DIR)/lparser.c \
	$(LUA_DIR)/lstate.c \
	$(LUA_DIR)/lstring.c \
	$(LUA_DIR)/lstrlib.c \
	$(LUA_DIR)/ltable.c \
	$(LUA_DIR)/ltablib.c \
	$(LUA_DIR)/ltm.c \
	$(LUA_DIR)/lundump.c \
	$(LUA_DIR)/lvm.c \
	$(LUA_DIR)/lzio.c

QSTAR_OBJS = $(QSTAR_SRCS:%.c=$(QSTAR_BUILD)/%.o)
LUA_OBJS = $(LUA_SRCS:%.c=$(QSTAR_BUILD)/%.o)
.PHONY: all check qstar-tests qstar-fmt-tests qstar-lint-tests qstar-lsp-tests qstar-lsp-navigation-tests qstar-editor-query-tests qstar-malformed-declaration-tests qstar-typed-dependency-target-tests qstar-reusable-command-set-tests qstar-composable-test-suite-tests qstar-ninja-backend-parity-tests qstar-generic-dsl-backend-parity-tests qstar-standard-provider-compatibility-tests qstar-real-glp-compiler-corpus-tests qstar-real-language-init-scaffold-tests qstar-medium-project-readiness-tests qstar-large-project-performance-tests qstar-perf-summary-tests qstar-performance-release-gate qstar-self-host-tests qstar-linux-validation-tests qstar-linux-daemon-validation-tests qstar-daemon-beta-boundary-tests qstar-windows-prep-tests qstar-windows-native-alpha-tests qstar-windows-execution-corpus-tests qstar-windows-sharedlib-artifact-parity-tests qstar-windows-release-package-tests qstar-windows-release-asset-smoke-tests qstar-public-beta-package qstar-public-beta-linux-package qstar-public-beta-github-upload qstar-public-beta-release-tests qstar-public-beta-download-smoke qstar-v0.8-release-tests qstar-v1-release-candidate-tests vscode-extension-tests qstar-v0-release-tests qstar-v0.1-release-tests qstar-v0.1-hardening-tests qstar-v0.2-authoring-tests qstar-v0.2-rc-tests qstar-v0.3-rc-tests qstar-v0.4-pilot-tests qstar-v0.5-readiness-tests qstar-pilot-readiness-tests qstar-wiki-cli-sync-tests qstar-release-candidate-tests qstar-full-regression-tests qstar-systems-corpus-tests qstar-project-corpus-tests qstar-standalone-integration-tests qstar-executor-v2-tests install clean

all: $(BIN_DIR)/qstar

$(BIN_DIR)/qstar: $(QSTAR_OBJS) $(LUA_OBJS)
	mkdir -p $(BIN_DIR)
	$(CC) $(QSTAR_OBJS) $(LUA_OBJS) $(LDLIBS) -o $@

$(QSTAR_BUILD)/src/%.o: src/%.c include/qstar/qstar.h src/internal.h
	mkdir -p $(QSTAR_BUILD)/src
	$(CC) $(QSTAR_CFLAGS) -c $< -o $@

$(QSTAR_BUILD)/$(LUA_DIR)/%.o: $(LUA_DIR)/%.c
	mkdir -p $(QSTAR_BUILD)/$(LUA_DIR)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

check: all
	set -e; \
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/smoke.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/malformed-declarations.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/typed-dependency-targets.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/reusable-command-sets.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/composable-test-suites.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/standard-provider-compatibility.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/wiki-cli-sync.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/daemon-beta-boundary.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/ninja-backend-parity.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/medium-project-performance.sh; \
	QSTAR_TEST_QSTAR="$$bin" QSTAR_LINUX_VALIDATION_CC="$${QSTAR_LINUX_VALIDATION_CC:-$(CC)}" sh tests/linux-validation.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/windows-prep.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/windows-native-alpha.sh; \
	QSTAR_TEST_QSTAR="$$bin" QSTAR_WINDOWS_EXECUTION_CC="$(CC)" sh tests/windows-execution-corpus.sh; \
	sh tests/windows-release-package.sh; \
	sh tests/windows-release-asset-smoke.sh

qstar-tests: check

qstar-fmt-tests: check

qstar-lint-tests: check

qstar-lsp-tests: check

qstar-lsp-navigation-tests: check

qstar-editor-query-tests: check

qstar-malformed-declaration-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/malformed-declarations.sh

qstar-typed-dependency-target-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/typed-dependency-targets.sh

qstar-reusable-command-set-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/reusable-command-sets.sh

qstar-composable-test-suite-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/composable-test-suites.sh

qstar-ninja-backend-parity-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/ninja-backend-parity.sh

qstar-generic-dsl-backend-parity-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/generic-dsl-backend-seal.sh

qstar-standard-provider-compatibility-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/standard-provider-compatibility.sh

qstar-real-glp-compiler-corpus-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/real-glp-compiler-corpus.sh

qstar-real-language-init-scaffold-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/real-language-init-scaffold.sh

qstar-medium-project-readiness-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/medium-project-performance.sh

qstar-large-project-performance-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/large-project-performance.sh

qstar-perf-summary-tests: all
	tmp="$${TMPDIR:-/tmp}/qstar-perf-summary-make.$$$$"; \
	rm -rf "$$tmp"; \
	mkdir -p "$$tmp"; \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	QSTAR_TEST_QSTAR="$(CURDIR)/$(BIN_DIR)/qstar" sh tests/medium-project-performance.sh > "$$tmp/medium.out"; \
	tools/perf-summary.sh "$$tmp/medium.out" > "$$tmp/summary.out"; \
	grep -F "perf_summary sample gate=medium mode=medium backend=stella phase=clean" "$$tmp/summary.out"; \
	grep -F "perf_summary ratio gate=medium mode=medium backend=stella phase=clean" "$$tmp/summary.out"; \
	tools/perf-summary.sh --format markdown "$$tmp/medium.out" > "$$tmp/summary.md"; \
	grep -F "| Gate | Mode | Backend | Phase | Count | Min ms | Median ms | Max ms | Skipped reason |" "$$tmp/summary.md"; \
	{ \
		printf '%s\n' 'medium_project_gate backend=stella phase=clean elapsed_ms=300'; \
		printf '%s\n' 'medium_project_gate backend=ninja phase=clean elapsed_ms=100'; \
		printf '%s\n' 'medium_project_gate backend=stella-daemon phase=clean elapsed_ms=skipped reason=socket-bind-not-permitted'; \
		printf '%s\n' 'large_project_gate mode=200 backend=stella phase=clean elapsed_ms=180'; \
		printf '%s\n' 'large_project_gate mode=200 backend=ninja phase=clean elapsed_ms=100'; \
	} > "$$tmp/synthetic.out"; \
	tools/perf-summary.sh --ratio-x100 150 --slack-ms 0 --hard-ratio-x100 250 --hard-slack-ms 0 "$$tmp/synthetic.out" > "$$tmp/synthetic-summary.out"; \
	grep -F "skipped_reason=socket-bind-not-permitted" "$$tmp/synthetic-summary.out"; \
	grep -F "hard_threshold_x100=250" "$$tmp/synthetic-summary.out"; \
	tools/perf-summary.sh --format markdown "$$tmp/synthetic.out" > "$$tmp/synthetic-summary.md"; \
	grep -F "| medium | medium | stella-daemon | clean | 1 | - | - | - | socket-bind-not-permitted |" "$$tmp/synthetic-summary.md"

qstar-performance-release-gate: all
	set -e; \
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	repeat="$${QSTAR_PERF_REPEAT:-3}"; \
	out_dir="$${QSTAR_PERF_ARTIFACT_DIR:-dist/perf}"; \
	mkdir -p "$$out_dir"; \
	medium_raw="$$out_dir/medium-release-raw.txt"; \
	large_raw="$$out_dir/large-release-raw.txt"; \
	: > "$$medium_raw"; \
	: > "$$large_raw"; \
	i=1; \
	while [ "$$i" -le "$$repeat" ]; do \
		printf 'qstar-performance-release-gate: medium run %s/%s\n' "$$i" "$$repeat"; \
		QSTAR_TEST_QSTAR="$$bin" sh tests/medium-project-performance.sh >> "$$medium_raw"; \
		i=$$((i + 1)); \
	done; \
	i=1; \
	while [ "$$i" -le "$$repeat" ]; do \
		printf 'qstar-performance-release-gate: large run %s/%s\n' "$$i" "$$repeat"; \
		QSTAR_TEST_QSTAR="$$bin" sh tests/large-project-performance.sh >> "$$large_raw"; \
		i=$$((i + 1)); \
	done; \
	tools/perf-summary.sh --label "QStar medium release performance" "$$medium_raw" > "$$out_dir/medium-release-summary.txt"; \
	tools/perf-summary.sh --format markdown --label "QStar medium release performance" "$$medium_raw" > "$$out_dir/medium-release-summary.md"; \
	tools/perf-summary.sh --label "QStar large release performance" "$$large_raw" > "$$out_dir/large-release-summary.txt"; \
	tools/perf-summary.sh --format markdown --label "QStar large release performance" "$$large_raw" > "$$out_dir/large-release-summary.md"; \
	printf 'qstar-performance-release-gate: artifacts in %s\n' "$$out_dir"

qstar-self-host-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/self-host.sh

qstar-linux-validation-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" QSTAR_LINUX_VALIDATION_CC="$${QSTAR_LINUX_VALIDATION_CC:-$(CC)}" sh tests/linux-validation.sh

qstar-linux-daemon-validation-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/linux-daemon-validation.sh

qstar-daemon-beta-boundary-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/daemon-beta-boundary.sh

qstar-windows-prep-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/windows-prep.sh

qstar-windows-native-alpha-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/windows-native-alpha.sh

qstar-windows-execution-corpus-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" QSTAR_WINDOWS_EXECUTION_CC="$(CC)" sh tests/windows-execution-corpus.sh

qstar-windows-sharedlib-artifact-parity-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/windows-sharedlib-artifact-parity.sh

qstar-windows-release-package-tests: all
	sh tests/windows-release-package.sh

qstar-windows-release-asset-smoke-tests: all
	sh tests/windows-release-asset-smoke.sh

qstar-public-beta-package: all
	$(SHELL) tools/package-public-beta.sh

qstar-public-beta-linux-package: all
	QSTAR_RELEASE_PLATFORM=linux-x86_64 $(SHELL) tools/package-public-beta.sh

qstar-public-beta-github-upload: all
	$(SHELL) tools/publish-github-release-asset.sh

qstar-public-beta-release-tests: qstar-public-beta-package

qstar-public-beta-download-smoke:
	$(SHELL) tools/smoke-github-release.sh

qstar-v0.8-release-tests: check qstar-generic-dsl-backend-parity-tests qstar-standard-provider-compatibility-tests qstar-real-glp-compiler-corpus-tests qstar-real-language-init-scaffold-tests qstar-performance-release-gate qstar-public-beta-package qstar-windows-release-package-tests qstar-windows-release-asset-smoke-tests
	git diff --check
	$(BIN_DIR)/qstar --version
	if [ "$${QSTAR_RUN_RELEASE_DOWNLOAD_SMOKE:-0}" = 1 ]; then \
		$(SHELL) tools/smoke-github-release.sh; \
	else \
		printf '%s\n' 'qstar-v0.8-release-tests: download-smoke=skipped reason=requires-published-release-asset'; \
	fi

qstar-v1-release-candidate-tests: check qstar-wiki-cli-sync-tests qstar-generic-dsl-backend-parity-tests qstar-standard-provider-compatibility-tests qstar-linux-validation-tests qstar-windows-prep-tests qstar-windows-native-alpha-tests qstar-windows-execution-corpus-tests qstar-windows-sharedlib-artifact-parity-tests qstar-windows-release-package-tests qstar-windows-release-asset-smoke-tests
	git diff --check
	$(BIN_DIR)/qstar --version
	grep -F "Status: Q256 stable DSL compatibility policy seal" docs/qstar-compatibility-policy.md
	grep -F "Three-OS Release Matrix Evidence Ledger" docs/release-matrix-evidence.md
	grep -F "Published GitHub Release asset" docs/release-matrix-evidence.md
	grep -F "qstar-v1-release-candidate-tests" docs/qstar-v1-readiness.md
	grep -F "qstar-v1-release-candidate-tests" README.md
	grep -F "qstar-v1-release-candidate-tests" README.ko.md
	grep -F "qstar-v1-release-candidate-tests" wiki/AI_INDEX.md
	grep -F "qstar-v1-release-candidate-tests" wiki/v1-readiness.md
	grep -F "qstar-v1-release-candidate-tests" man/man1/qstar.1
	if [ "$${QSTAR_RUN_RELEASE_DOWNLOAD_SMOKE:-0}" = 1 ]; then \
		$(SHELL) tools/smoke-github-release.sh; \
	else \
		printf '%s\n' 'qstar-v1-release-candidate-tests: release-download-smoke=skipped reason=requires-published-release-asset'; \
	fi

vscode-extension-tests:
	node editors/vscode/qstar/scripts/check-package.js editors/vscode/qstar

qstar-v0-release-tests: check

qstar-v0.1-release-tests: check

qstar-v0.1-hardening-tests: check

qstar-v0.2-authoring-tests: check

qstar-v0.2-rc-tests: check

qstar-v0.3-rc-tests: check

qstar-v0.4-pilot-tests: check

qstar-v0.5-readiness-tests: check

qstar-pilot-readiness-tests: check

qstar-wiki-cli-sync-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/wiki-cli-sync.sh

qstar-release-candidate-tests: qstar-v1-release-candidate-tests

qstar-full-regression-tests: check

qstar-systems-corpus-tests: check

qstar-project-corpus-tests: check

qstar-standalone-integration-tests: check

qstar-executor-v2-tests: check

install: all
	mkdir -p "$(PREFIX)/bin"
	if [ -f "$(BIN_DIR)/qstar" ]; then \
		cp "$(BIN_DIR)/qstar" "$(PREFIX)/bin/qstar"; \
	elif [ -f "$(BIN_DIR)/qstar.exe" ]; then \
		cp "$(BIN_DIR)/qstar.exe" "$(PREFIX)/bin/qstar"; \
	else \
		printf '%s\n' "missing built qstar binary under $(BIN_DIR)" >&2; \
		exit 1; \
	fi
	if [ -f "$(BIN_DIR)/qstar.exe" ]; then \
		cp "$(BIN_DIR)/qstar.exe" "$(PREFIX)/bin/qstar.exe"; \
	fi
	if command -v codesign >/dev/null 2>&1 && [ "$$(uname -s)" = Darwin ]; then \
		codesign --force --sign - "$(PREFIX)/bin/qstar"; \
	fi
	mkdir -p "$(DOC_DIR)"
	rm -rf "$(DOC_DIR)/wiki"
	cp -R wiki "$(DOC_DIR)/wiki"
	mkdir -p "$(PROVIDER_DIR)"
	for provider in qstar/languages/*; do \
		name=$${provider##*/}; \
		rm -rf "$(PROVIDER_DIR)/$$name"; \
		cp -R "$$provider" "$(PROVIDER_DIR)/$$name"; \
	done
	mkdir -p "$(MAN_DIR)/man1" "$(MAN_DIR)/man5"
	cp man/man1/qstar.1 "$(MAN_DIR)/man1/qstar.1"
	cp man/man5/qstar-lua.5 "$(MAN_DIR)/man5/qstar-lua.5"

clean:
	rm -rf $(QSTAR_BUILD) $(BIN_DIR)/qstar $(BIN_DIR)/qstar.exe
