# ── Xvsh Makefile ─────────────────────────────────────────────────
#
# Dependencies (Ubuntu/Debian):
#   sudo apt install liblua5.4-dev libreadline-dev
#
# Build:   make
# Install: sudo make install
# Clean:   make clean

CC      := gcc
TARGET  := xvsh

# Source files
SRCDIR  := src
SRCS    := $(SRCDIR)/main.c \
           $(SRCDIR)/lexer.c \
           $(SRCDIR)/parser.c \
           $(SRCDIR)/executor.c \
           $(SRCDIR)/builtins.c \
           $(SRCDIR)/lua_config.c
OBJS    := $(SRCS:.c=.o)

# Flags
LUA_CFLAGS  := $(shell pkg-config --cflags lua5.4 2>/dev/null || echo "-I/usr/include/lua5.4")
LUA_LIBS    := $(shell pkg-config --libs   lua5.4 2>/dev/null || echo "-llua5.4")

CFLAGS  := -Wall -Wextra -Wpedantic -std=c11 -g -O2 $(LUA_CFLAGS) -I$(SRCDIR)
LDFLAGS := $(LUA_LIBS) -lreadline

# ── Default target ─────────────────────────────────────────────────
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Built $(TARGET) successfully."

# ── Compile rule ───────────────────────────────────────────────────
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── Header dependencies ────────────────────────────────────────────
$(SRCDIR)/main.o:       $(SRCDIR)/xvsh.h $(SRCDIR)/lexer.h $(SRCDIR)/parser.h \
                        $(SRCDIR)/executor.h $(SRCDIR)/builtins.h $(SRCDIR)/lua_config.h
$(SRCDIR)/lexer.o:      $(SRCDIR)/xvsh.h $(SRCDIR)/lexer.h
$(SRCDIR)/parser.o:     $(SRCDIR)/xvsh.h $(SRCDIR)/lexer.h $(SRCDIR)/parser.h
$(SRCDIR)/executor.o:   $(SRCDIR)/xvsh.h $(SRCDIR)/lexer.h $(SRCDIR)/parser.h \
                        $(SRCDIR)/executor.h $(SRCDIR)/builtins.h $(SRCDIR)/lua_config.h
$(SRCDIR)/builtins.o:   $(SRCDIR)/xvsh.h $(SRCDIR)/builtins.h $(SRCDIR)/lua_config.h
$(SRCDIR)/lua_config.o: $(SRCDIR)/xvsh.h $(SRCDIR)/lua_config.h

# ── Install ────────────────────────────────────────────────────────
PREFIX  ?= /usr/local
install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@echo "Installed to $(PREFIX)/bin/$(TARGET)"

# ── Uninstall ──────────────────────────────────────────────────────
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

# ── Clean ──────────────────────────────────────────────────────────
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install uninstall clean
