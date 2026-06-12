.POSIX:

CC ?= cc
BUILD_DIR ?= build
BIN_DIR ?= $(BUILD_DIR)/bin
PREFIX ?= /usr/local
DOC_DIR ?= $(PREFIX)/share/doc/qstar
MAN_DIR ?= $(PREFIX)/share/man
QSTAR_BUILD = $(BUILD_DIR)/obj
LUA_DIR = vendor/lua
CFLAGS ?= -g -O0 -pipe
QSTAR_CFLAGS = -std=c99 -Wall -Wextra -Wpedantic $(CFLAGS) -Iinclude -I$(LUA_DIR)
LUA_CFLAGS = -std=c99 -O2 -I$(LUA_DIR)

QSTAR_SRCS = \
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
	src/plan.c \
	src/profile.c \
	src/rule.c \
	src/source.c \
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
.PHONY: all check qstar-tests qstar-fmt-tests qstar-lint-tests qstar-lsp-tests qstar-lsp-navigation-tests qstar-editor-query-tests qstar-ninja-backend-parity-tests qstar-medium-project-readiness-tests qstar-self-host-tests qstar-linux-validation-tests qstar-windows-prep-tests qstar-public-beta-package qstar-public-beta-release-tests vscode-extension-tests qstar-v0-release-tests qstar-v0.1-release-tests qstar-v0.1-hardening-tests qstar-v0.2-authoring-tests qstar-v0.2-rc-tests qstar-v0.3-rc-tests qstar-v0.4-pilot-tests qstar-v0.5-readiness-tests qstar-pilot-readiness-tests qstar-wiki-cli-sync-tests qstar-release-candidate-tests qstar-full-regression-tests qstar-systems-corpus-tests qstar-project-corpus-tests qstar-standalone-integration-tests qstar-executor-v2-tests install clean

all: $(BIN_DIR)/qstar

$(BIN_DIR)/qstar: $(QSTAR_OBJS) $(LUA_OBJS)
	mkdir -p $(BIN_DIR)
	$(CC) $(QSTAR_OBJS) $(LUA_OBJS) -o $@

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
	QSTAR_TEST_QSTAR="$$bin" sh tests/ninja-backend-parity.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/medium-project-performance.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/linux-validation.sh; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/windows-prep.sh

qstar-tests: check

qstar-fmt-tests: check

qstar-lint-tests: check

qstar-lsp-tests: check

qstar-lsp-navigation-tests: check

qstar-editor-query-tests: check

qstar-ninja-backend-parity-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/ninja-backend-parity.sh

qstar-medium-project-readiness-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/medium-project-performance.sh

qstar-self-host-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/self-host.sh

qstar-linux-validation-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/linux-validation.sh

qstar-windows-prep-tests: all
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/windows-prep.sh

qstar-public-beta-package: all
	$(SHELL) tools/package-public-beta.sh

qstar-public-beta-release-tests: qstar-public-beta-package

vscode-extension-tests: check

qstar-v0-release-tests: check

qstar-v0.1-release-tests: check

qstar-v0.1-hardening-tests: check

qstar-v0.2-authoring-tests: check

qstar-v0.2-rc-tests: check

qstar-v0.3-rc-tests: check

qstar-v0.4-pilot-tests: check

qstar-v0.5-readiness-tests: check

qstar-pilot-readiness-tests: check

qstar-wiki-cli-sync-tests: check

qstar-release-candidate-tests: check

qstar-full-regression-tests: check

qstar-systems-corpus-tests: check

qstar-project-corpus-tests: check

qstar-standalone-integration-tests: check

qstar-executor-v2-tests: check

install: all
	mkdir -p "$(PREFIX)/bin"
	cp "$(BIN_DIR)/qstar" "$(PREFIX)/bin/qstar"
	if command -v codesign >/dev/null 2>&1 && [ "$$(uname -s)" = Darwin ]; then \
		codesign --force --sign - "$(PREFIX)/bin/qstar"; \
	fi
	mkdir -p "$(DOC_DIR)"
	rm -rf "$(DOC_DIR)/wiki"
	cp -R wiki "$(DOC_DIR)/wiki"
	mkdir -p "$(MAN_DIR)/man1" "$(MAN_DIR)/man5"
	cp man/man1/qstar.1 "$(MAN_DIR)/man1/qstar.1"
	cp man/man5/qstar-lua.5 "$(MAN_DIR)/man5/qstar-lua.5"

clean:
	rm -rf $(QSTAR_BUILD) $(BIN_DIR)/qstar
