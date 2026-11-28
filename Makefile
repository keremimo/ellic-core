# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
# The core's own tests (tests/core_tests.cpp) against the core's sources that
# need no JUCE, on Linux: `make test`, from tests/test_core.py or a product's
# build (./build.sh in melodee-sample and melodee-move run it in their build
# images). Products build the core through core.mk, never through this file.
CORE := .
include core.mk
CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -pthread -Wall -Wextra -Werror -MMD -MP
JUCE_SOURCES := $(shell grep -l juce $(CORE_SOURCES))
TEST_SOURCES := $(filter-out $(JUCE_SOURCES),$(CORE_SOURCES)) tests/core_tests.cpp
TEST_OBJECTS := $(patsubst %.cpp,build/obj/%.o,$(TEST_SOURCES))

.PHONY: test
test: build/core-tests
	build/core-tests
build/core-tests: $(TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@
build/obj/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CORE_CPPFLAGS) $(CXXFLAGS) -c $< -o $@
-include $(TEST_OBJECTS:.o=.d)
