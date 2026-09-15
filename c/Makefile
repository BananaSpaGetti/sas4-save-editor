# SAS4 save-editor core, ported to C -- builds libsas4core.a plus any main_*.c CLI entry
# points that live in src/ (see the port-to-c-sas4-core plan for what belongs here; the
# full sas4.py CLI is a separate follow-on plan).
#
# COMMON_SRCS is every src/*.c file except the main_*.c entry points -- the same split
# MemScope/c/Makefile uses, so the library and every CLI executable share one set of
# objects. CFLAGS asks gcc to emit a .d file beside each .o listing the headers it
# included, and those .d files are included below so that editing a header rebuilds
# every object that includes it, not just the .c files that changed.

CC      := gcc
AR      := ar
CFLAGS  := -std=c11 -Wall -Wextra -O2 -Iinclude -MMD -MP
LDLIBS  := -lkernel32 -ladvapi32 -lwinhttp

BUILD_DIR := build
LIB       := libsas4core.a

ALL_SRCS    := $(wildcard src/*.c)
MAIN_SRCS   := $(wildcard src/main_*.c)
COMMON_SRCS := $(filter-out $(MAIN_SRCS),$(ALL_SRCS))
COMMON_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))
MAIN_OBJS   := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(MAIN_SRCS))
MAIN_BINS   := $(patsubst src/main_%.c,%.exe,$(MAIN_SRCS))

TEST_SRCS := $(wildcard tests/test_*.c)
TEST_OBJS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.exe,$(TEST_SRCS))

COMMON_DEPS := $(COMMON_OBJS:.o=.d) $(MAIN_OBJS:.o=.d)
TEST_DEPS   := $(TEST_OBJS:.o=.d)

.PHONY: all clean test

all: $(LIB) $(MAIN_BINS)

$(LIB): $(COMMON_OBJS)
	$(AR) rcs $@ $^

%.exe: $(BUILD_DIR)/main_%.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/tests/%.o: tests/%.c | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/tests/%.exe: $(BUILD_DIR)/tests/%.o $(COMMON_OBJS) | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR) $(BUILD_DIR)/tests:
	mkdir -p $@

test: $(TEST_BINS)
ifeq ($(TEST_SRCS),)
	@echo "no C tests yet"
else
	@for t in $(TEST_BINS); do \
		echo "== $$t =="; \
		"$$t" || exit 1; \
	done
endif

clean:
	rm -rf $(BUILD_DIR) $(LIB) $(MAIN_BINS)

-include $(COMMON_DEPS)
-include $(TEST_DEPS)
