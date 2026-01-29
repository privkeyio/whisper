# whisper - Encrypted DM pipe for Nostr
#
# Prerequisites:
#   libnostr-c built with NIP-17 enabled:
#     cd ../libnostr-c/build && cmake .. -DNOSTR_FEATURE_NIP17=ON -DNOSTR_FEATURE_RELAY=ON && make

CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_DEFAULT_SOURCE
LDFLAGS =

# libnostr-c paths (adjust if needed)
LIBNOSTR_DIR ?= ../libnostr-c
LIBNOSTR_INC = $(LIBNOSTR_DIR)/include $(LIBNOSTR_DIR)/build/include
LIBNOSTR_LIB = $(LIBNOSTR_DIR)/build/libnostr.a

# Dependencies (from pkg-config or manual)
PKG_DEPS = libcjson libsecp256k1

# Try pkg-config first
PKG_CFLAGS := $(shell pkg-config --cflags $(PKG_DEPS) 2>/dev/null)
PKG_LIBS := $(shell pkg-config --libs $(PKG_DEPS) 2>/dev/null)

# Add noscrypt if available
NOSCRYPT_CFLAGS := $(shell pkg-config --cflags noscrypt 2>/dev/null)
NOSCRYPT_LIBS := $(shell pkg-config --libs noscrypt 2>/dev/null)

# WebSocket library (libwebsockets or cwebsocket)
WS_CFLAGS := $(shell pkg-config --cflags libwebsockets 2>/dev/null)
WS_LIBS := $(shell pkg-config --libs libwebsockets 2>/dev/null)

# If no pkg-config for websockets, try cwebsocket
ifeq ($(WS_LIBS),)
WS_LIBS = -lcwebsocket -lssl -lcrypto
endif

# Notcurses for TUI (optional)
NOTCURSES_CFLAGS := $(shell pkg-config --cflags notcurses-core 2>/dev/null)
NOTCURSES_LIBS := $(shell pkg-config --libs notcurses-core 2>/dev/null)

# Fall back to local notcurses if not in system
NOTCURSES_LOCAL = ../notcurses/build/libnotcurses-core.a
NOTCURSES_LOCAL_INC = ../notcurses/include
NOTCURSES_LOCAL_EXISTS := $(wildcard $(NOTCURSES_LOCAL))

ifeq ($(NOTCURSES_LIBS),)
ifneq ($(NOTCURSES_LOCAL_EXISTS),)
NOTCURSES_CFLAGS = -I$(NOTCURSES_LOCAL_INC) -DHAVE_NOTCURSES
NOTCURSES_LIBS = $(NOTCURSES_LOCAL) -lunistring -lz -ltinfo
endif
else
NOTCURSES_CFLAGS += -DHAVE_NOTCURSES
endif

# OpenSSL (required by noscrypt/nip44)
SSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")

# noscrypt local path (if not in system)
NOSCRYPT_LOCAL = ../noscrypt/build/libnoscrypt_static.a
NOSCRYPT_MONOCYPHER = ../noscrypt/build/libmonocypher.a
NOSCRYPT_INC = ../noscrypt/include

# Combine flags
CFLAGS += $(addprefix -I,$(LIBNOSTR_INC)) $(PKG_CFLAGS) $(NOSCRYPT_CFLAGS) $(WS_CFLAGS) $(NOTCURSES_CFLAGS)

# Use local noscrypt static libs only if both exist
NOSCRYPT_LOCAL_EXISTS := $(wildcard $(NOSCRYPT_LOCAL))
NOSCRYPT_MONOCYPHER_EXISTS := $(wildcard $(NOSCRYPT_MONOCYPHER))
USE_LOCAL_NOSCRYPT := $(and $(NOSCRYPT_LOCAL_EXISTS),$(NOSCRYPT_MONOCYPHER_EXISTS))

ifneq (,$(USE_LOCAL_NOSCRYPT))
CFLAGS += -I$(NOSCRYPT_INC)
LIBS = $(LIBNOSTR_LIB) $(NOSCRYPT_LOCAL) $(NOSCRYPT_MONOCYPHER) $(PKG_LIBS) $(WS_LIBS) $(NOTCURSES_LIBS) $(SSL_LIBS) -lpthread -lm
else
LIBS = $(LIBNOSTR_LIB) $(PKG_LIBS) $(NOSCRYPT_LIBS) $(WS_LIBS) $(NOTCURSES_LIBS) $(SSL_LIBS) -lpthread -lm
endif

# Source files
SRCS = main.c send.c recv.c util.c tui.c
OBJS = $(SRCS:.c=.o)
TARGET = whisper

.PHONY: all clean install check-deps

all: check-deps $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c whisper.h tui.h
	$(CC) $(CFLAGS) -c -o $@ $<

check-deps:
	@if [ ! -f "$(LIBNOSTR_LIB)" ]; then \
		echo "Error: libnostr.a not found at $(LIBNOSTR_LIB)"; \
		echo ""; \
		echo "Build libnostr-c with NIP-17 and relay support:"; \
		echo "  cd $(LIBNOSTR_DIR)/build"; \
		echo "  cmake .. -DNOSTR_FEATURE_NIP17=ON -DNOSTR_FEATURE_RELAY=ON"; \
		echo "  make"; \
		exit 1; \
	fi
	@if [ -f "$(NOSCRYPT_LOCAL)" ] && [ ! -f "$(NOSCRYPT_MONOCYPHER)" ]; then \
		echo "Error: libmonocypher.a not found at $(NOSCRYPT_MONOCYPHER)"; \
		echo "Rebuild noscrypt: cd ../noscrypt/build && cmake .. && make"; \
		exit 1; \
	fi
	@if [ ! -f "$(NOSCRYPT_LOCAL)" ] && [ -f "$(NOSCRYPT_MONOCYPHER)" ]; then \
		echo "Error: libnoscrypt_static.a not found at $(NOSCRYPT_LOCAL)"; \
		echo "Rebuild noscrypt: cd ../noscrypt/build && cmake .. && make"; \
		exit 1; \
	fi

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Development helpers
run-send: $(TARGET)
	echo "test message" | ./$(TARGET) send --to npub1test --nsec nsec1test --relay wss://relay.damus.io

run-recv: $(TARGET)
	./$(TARGET) recv --nsec nsec1test --relay wss://relay.damus.io --limit 5

run-tui: $(TARGET)
	./$(TARGET) tui --relay wss://relay.damus.io
