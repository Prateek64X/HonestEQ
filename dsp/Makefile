# Plain Makefile for the DSP library tests — no CMake / Xcode required.
# Useful for quickly verifying correctness in any C++17 environment.
# The "real" build on macOS goes through Xcode + xcodebuild.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow

# Detect host so binaries from a different OS/arch get rebuilt automatically.
# (Without this, a Linux binary copied into build/ from a sandbox can fool make
# into thinking the target is up-to-date on macOS.)
HOST_TAG := $(shell uname -sm | tr ' /' '__')
ARCH_STAMP := build/.host-$(HOST_TAG)

INCLUDE_TEST := -Iinclude -Itests
INCLUDE_TOOL := -Iinclude

TEST_SRC := tests/test_biquad.cpp
TEST_OUT := build/honesteq_dsp_tests

TOOL_SRC := tools/honesteq-render.cpp
TOOL_OUT := build/honesteq-render

HEADERS := include/honesteq/Biquad.hpp include/honesteq/BiquadChain.hpp \
           include/honesteq/Wav.hpp include/honesteq/PeaceConfig.hpp

.PHONY: all test tool clean
all: $(TEST_OUT) $(TOOL_OUT)

$(ARCH_STAMP):
	@mkdir -p build
	@rm -f build/.host-* build/honesteq_dsp_tests build/honesteq-render 2>/dev/null || true
	@touch $(ARCH_STAMP)

$(TEST_OUT): $(ARCH_STAMP) $(TEST_SRC) $(HEADERS) tests/MicroTest.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDE_TEST) $(TEST_SRC) -o $(TEST_OUT)

$(TOOL_OUT): $(ARCH_STAMP) $(TOOL_SRC) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDE_TOOL) $(TOOL_SRC) -o $(TOOL_OUT)

test: $(TEST_OUT)
	$(TEST_OUT)

tool: $(TOOL_OUT)

clean:
	rm -rf build
