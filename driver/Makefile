# HonestEQ driver — builds the .driver CFPlugIn bundle.
#
# Bundle layout produced by `make`:
#   build/HonestEQ.driver/
#     Contents/
#       Info.plist
#       MacOS/HonestEQ            (Mach-O bundle, links CoreAudio + CoreFoundation)
#       _CodeSignature/...        (added by codesign at the install step)

BUNDLE_NAME  := HonestEQ.driver
BUILD_DIR    := build
BUNDLE_DIR   := $(BUILD_DIR)/$(BUNDLE_NAME)
BIN_DIR      := $(BUNDLE_DIR)/Contents/MacOS
BIN          := $(BIN_DIR)/HonestEQ
PLIST_SRC    := Info.plist
PLIST_DST    := $(BUNDLE_DIR)/Contents/Info.plist

CC           ?= cc
CFLAGS       := -arch arm64 -arch x86_64 \
                -O2 -fvisibility=hidden \
                -Wall -Wextra -Wno-unused-parameter \
                -mmacosx-version-min=11.0
LDFLAGS      := -bundle \
                -framework CoreAudio \
                -framework CoreFoundation

SOURCES      := HonestEQDriver.c

.PHONY: all clean
all: $(BIN) $(PLIST_DST)

$(BIN): $(SOURCES) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)
	@echo "Built $@"

$(PLIST_DST): $(PLIST_SRC) | $(BUNDLE_DIR)/Contents
	cp $(PLIST_SRC) $(PLIST_DST)

$(BIN_DIR) $(BUNDLE_DIR)/Contents:
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
