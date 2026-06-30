CC      = clang
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -framework CoreAudio -framework AudioToolbox -framework CoreFoundation

BIN    = xlrbridge
SRC    = $(wildcard src/*.c)
OBJ    = $(SRC:.c=.o)

# Install location. Override with `make install PREFIX=/opt/homebrew`.
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN) $(OBJ)
