BIN      := build/honesteq-daemon
CC       ?= cc
CFLAGS   := -arch arm64 -arch x86_64 -O2 -Wall -Wextra -Wno-unused-parameter \
            -mmacosx-version-min=11.0
LDFLAGS  := -framework CoreAudio -framework AudioUnit -framework AudioToolbox \
            -framework CoreFoundation

.PHONY: all clean
all: $(BIN)

$(BIN): main.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BIN) main.c

clean:
	rm -rf build
