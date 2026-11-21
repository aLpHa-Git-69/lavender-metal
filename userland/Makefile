# Host tests for the A5xx layer (no device). `make test`; `make regen` rewrites a5xx.hpp from refs/ext (Mesa XML).
CXX = clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Iinclude

test: build/cp_test
	build/cp_test

build/cp_test: tests/cp_test.cpp include/lavender-metal/*.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) tests/cp_test.cpp -o $@

regen:
	cd .. && python3 tools/gen_a5xx.py

.PHONY: test regen
