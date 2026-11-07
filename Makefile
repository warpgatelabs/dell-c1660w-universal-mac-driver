# SPDX-License-Identifier: Apache-2.0

ifeq ($(origin CC),default)
CC      := clang
endif
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra
BUILD   := build

HBPL_VECTORS ?=
VECTOR_FILES := $(if $(HBPL_VECTORS),$(sort $(wildcard $(HBPL_VECTORS)/*.prn)))

LIB_OBJS := $(BUILD)/hbpl.o $(BUILD)/hbpl_decode.o
TOOLS    := $(BUILD)/hbpl-encode $(BUILD)/hbpl-decode
TESTS    := $(BUILD)/test_codes $(BUILD)/test_roundtrip $(BUILD)/test_framing \
            $(BUILD)/test_vectors

.PHONY: all app test clean

all: $(LIB_OBJS) $(TOOLS)

# The Printer Application needs PAPPL (https://www.msweet.org/pappl). Point PKG_CONFIG_PATH
# at its pappl.pc, or set PAPPL_PREFIX to where it is installed.
PAPPL_PREFIX ?= /usr/local
PAPPL_PKG    := PKG_CONFIG_PATH="$(PAPPL_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config
APP_SRCS     := app/c1660w-universal-mac-driver.c app/color.c src/hbpl.c

app: $(BUILD)/c1660w-universal-mac-driver

$(BUILD)/c1660w-universal-mac-driver: $(APP_SRCS) app/color.h app/color_tables.h src/hbpl.h | $(BUILD)
	$(CC) $(CFLAGS) -D_DARWIN_C_SOURCE -Wno-deprecated-declarations $$($(PAPPL_PKG) --cflags pappl) \
	    -o $@ $(APP_SRCS) $$($(PAPPL_PKG) --libs --static pappl)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/hbpl.o: src/hbpl.c src/hbpl.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/hbpl_decode.o: src/hbpl_decode.c src/hbpl_decode.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/hbpl-encode: tools/hbpl-encode.c $(LIB_OBJS) src/hbpl.h
	$(CC) $(CFLAGS) -Isrc -o $@ $< $(LIB_OBJS)

$(BUILD)/hbpl-decode: tools/hbpl-decode.c $(LIB_OBJS) src/hbpl_decode.h
	$(CC) $(CFLAGS) -Isrc -o $@ $< $(LIB_OBJS)

# test_codes includes the library sources to reach the static code-table helpers.
$(BUILD)/test_codes: tests/test_codes.c tests/test_util.h src/hbpl.c src/hbpl_decode.c \
                     src/hbpl.h src/hbpl_decode.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ $<

$(BUILD)/test_%: tests/test_%.c tests/test_util.h $(LIB_OBJS)
	$(CC) $(CFLAGS) -Isrc -o $@ $< $(LIB_OBJS)

test: all $(TESTS)
	$(BUILD)/test_codes
	$(BUILD)/test_roundtrip
	@echo "$(BUILD)/test_framing <$(words $(VECTOR_FILES)) vectors>"
	@$(BUILD)/test_framing $(VECTOR_FILES)
	@echo "$(BUILD)/test_vectors <$(words $(VECTOR_FILES)) vectors>"
	@$(BUILD)/test_vectors $(VECTOR_FILES)

clean:
	rm -rf $(BUILD)
