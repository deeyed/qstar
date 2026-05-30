.POSIX:

CC ?= cc
BUILD_DIR ?= ../build
BIN_DIR ?= $(BUILD_DIR)/bin
QSTAR_BUILD = $(BUILD_DIR)/qstar
LUA_DIR = vendor/lua
CFLAGS ?= -g -O0 -pipe
QSTAR_CFLAGS = -std=c99 -Wall -Wextra -Wpedantic $(CFLAGS) -Iinclude -I$(LUA_DIR)
LUA_CFLAGS = -std=c99 -O2 -I$(LUA_DIR)

QSTAR_SRCS = \
	src/executor.c \
	src/graph.c \
	src/header.c \
	src/label.c \
	src/lua_runtime.c \
	src/plan.c \
	src/profile.c \
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
QSTAR_TEST = $(QSTAR_BUILD)/tests/qstar-tests
QSTAR_TEST_SRCS = \
	../tools/qstar_tests.c \
	../tools/test_runtime.c \
	../host/src/generic/path.c \
	../host/src/generic/sdk.c \
	../host/src/posix/env.c \
	../host/src/posix/executable.c \
	../host/src/posix/fs.c \
	../host/src/posix/process.c \
	../host/src/posix/temp.c

.PHONY: all check clean

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

$(QSTAR_TEST): $(QSTAR_TEST_SRCS)
	mkdir -p $(QSTAR_BUILD)/tests
	$(CC) -std=c99 -Wall -Wextra -Wpedantic $(CFLAGS) -I../host/include $^ -o $@

check: all $(QSTAR_TEST)
	bin="$(BIN_DIR)/qstar"; testbin="$(QSTAR_TEST)"; \
	case "$$bin" in /*) ;; *) bin="$(CURDIR)/$$bin";; esac; \
	case "$$testbin" in /*) ;; *) testbin="$(CURDIR)/$$testbin";; esac; \
	cd "$(CURDIR)/.." && QSTAR_TEST_QSTAR="$$bin" "$$testbin" all

clean:
	rm -rf $(QSTAR_BUILD) $(BIN_DIR)/qstar
