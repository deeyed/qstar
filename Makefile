.POSIX:

CC ?= cc
BUILD_DIR ?= build
BIN_DIR ?= $(BUILD_DIR)/bin
PREFIX ?= /usr/local
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
.PHONY: all check qstar-tests qstar-fmt-tests qstar-lint-tests qstar-lsp-tests qstar-lsp-navigation-tests qstar-editor-query-tests vscode-extension-tests qstar-v0-release-tests qstar-v0.1-release-tests qstar-v0.1-hardening-tests qstar-v0.2-authoring-tests qstar-project-corpus-tests qstar-standalone-integration-tests qstar-executor-v2-tests install clean

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
	bin="$(BIN_DIR)/qstar"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	QSTAR_TEST_QSTAR="$$bin" sh tests/smoke.sh

qstar-tests: check

qstar-fmt-tests: check

qstar-lint-tests: check

qstar-lsp-tests: check

qstar-lsp-navigation-tests: check

qstar-editor-query-tests: check

vscode-extension-tests: check

qstar-v0-release-tests: check

qstar-v0.1-release-tests: check

qstar-v0.1-hardening-tests: check

qstar-v0.2-authoring-tests: check

qstar-project-corpus-tests: check

qstar-standalone-integration-tests: check

qstar-executor-v2-tests: check

install: all
	mkdir -p "$(PREFIX)/bin"
	cp "$(BIN_DIR)/qstar" "$(PREFIX)/bin/qstar"

clean:
	rm -rf $(QSTAR_BUILD) $(BIN_DIR)/qstar
