# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
# ellic-core in a product's build: the product's Makefile sets CORE (where
# this checkout is, by default the core/ submodule), includes this file and
# compiles CORE_SOURCES with CORE_CPPFLAGS among its own flags. Nothing here
# compiles anything: each product keeps its own compiler, flags and object
# folders.
#
# What the core needs of the product's build:
#   JUCE's juce_core and juce_audio_formats (XML, JSON, files; the readers of
#     AIFF, FLAC, Ogg and MP3 in shell/sample_decode.cpp), with the module
#     settings the product already uses.
#   Linux (mmap, futexes, sockets for web/web_server.cpp), C++17, -pthread.
#   CORE_ASFLAGS on shell/mpc3_export.cpp: its template is assembled in with
#     .incbin, found relative to the checkout; a product that lists object
#     dependencies makes that object depend on CORE_TEMPLATE.
CORE ?= core
CORE_SOURCES := $(sort $(wildcard $(CORE)/src/melodee/*/*.cpp))
CORE_HEADERS := $(sort $(wildcard $(CORE)/src/melodee/*/*.h))
CORE_CPPFLAGS := -I$(CORE)/src
CORE_ASFLAGS := -Wa,-I$(CORE)
CORE_TEMPLATE := $(CORE)/src/melodee/shell/mpc3_template.json
