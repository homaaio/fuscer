CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -Wpedantic
LDLIBS  += -lpthread

BUILD   := build
LIB_SRC := $(filter-out src/main.c,$(wildcard src/*.c))
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o)
MAIN_OBJ := $(BUILD)/main.o

all: $(BUILD)/fuscer

$(BUILD)/fuscer: $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/test_proto: tests/test_proto.c $(LIB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ $^ $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/fuscer $(BUILD)/test_proto
	$(BUILD)/test_proto
	sh tests/smoke.sh $(BUILD)/fuscer

install: $(BUILD)/fuscer
	install -Dm755 $(BUILD)/fuscer $(DESTDIR)/usr/local/bin/fuscer

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)

.PHONY: all test install clean
