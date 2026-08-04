# Host build of the layout. No ESP-IDF involved — this is why Layout.cpp, Gfx.cpp
# and fonts.cpp are kept free of esp_* includes.
#
#   make && ./preview && open out-*.ppm

CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter

EPAPER = ../../components/epaper
MAIN   = ../../main

INCLUDES = -I$(EPAPER)/include -I$(EPAPER)/fonts -I$(MAIN)

SRCS = preview.cpp \
       $(MAIN)/Layout.cpp \
       $(EPAPER)/src/Gfx.cpp \
       $(EPAPER)/fonts/fonts.cpp

preview: $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(SRCS)

.PHONY: run clean
run: preview
	./preview

clean:
	rm -f preview out-*.ppm out-*.pbm
