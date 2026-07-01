BIN      := build/honesteq-daemon
CC       ?= cc
CXX      ?= c++
COMMON   := -arch arm64 -arch x86_64 -O2 -Wall -Wextra -Wno-unused-parameter \
            -mmacosx-version-min=11.0
CFLAGS   := $(COMMON)
CXXFLAGS := $(COMMON) -std=c++17 -I../dsp/include
LDFLAGS  := -framework CoreAudio -framework AudioUnit -framework AudioToolbox \
            -framework CoreFoundation

C_SRC    := main.c
CPP_SRC  := EqBridge.cpp
HEADERS  := EqBridge.hpp

C_OBJS   := $(patsubst %.c,build/%.o,$(C_SRC))
CPP_OBJS := $(patsubst %.cpp,build/%.o,$(CPP_SRC))
OBJS     := $(C_OBJS) $(CPP_OBJS)

.PHONY: all clean
all: $(BIN)

build/%.o: %.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) -o $(BIN)

clean:
	rm -rf build
