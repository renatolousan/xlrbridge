CC      = clang
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -framework CoreAudio -framework AudioToolbox -framework CoreFoundation

BIN  = xlrbridge
SRC  = $(wildcard src/*.c)
OBJ  = $(SRC:.c=.o)

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJ)
