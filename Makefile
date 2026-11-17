# Cairn build. Plain make + MinGW-w64 gcc; no CMake, no Visual Studio.
#
#   make            build build/cairn.exe (Windows, static, GUI)
#   make test       build and run the UI-free core tests natively (needs a host gcc)
#   make clean
#
# On Windows with MinGW-w64 on PATH, run with:  make CROSS=
# On Linux, the default CROSS prefix targets the x86_64 MinGW-w64 cross compiler.

CROSS   ?= x86_64-w64-mingw32-
CXX     := $(CROSS)g++
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres

BUILD := build
OBJ   := $(BUILD)/win

SQLITE_FLAGS := -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_FTS5 -DSQLITE_OMIT_LOAD_EXTENSION \
                -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
CXXFLAGS     := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc/core -Isrc/win -Ivendor/sqlite \
                -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00
LDFLAGS      := -static -mwindows -municode
LIBS         := -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -lole32 -lwindowscodecs -luuid -lmsimg32 -lwinmm

CORE_SRC := src/core/document.cpp src/core/model.cpp src/core/rich.cpp src/core/export.cpp src/core/markdown.cpp src/core/table.cpp src/core/media.cpp src/core/script.cpp src/core/arrange.cpp
WIN_SRC  := src/win/main.cpp src/win/canvas.cpp src/win/propform.cpp src/win/images.cpp
OBJS     := $(patsubst src/%.cpp,$(OBJ)/%.o,$(CORE_SRC) $(WIN_SRC)) $(OBJ)/sqlite3.o $(OBJ)/cairn_res.o

all: $(BUILD)/cairn.exe

$(BUILD)/cairn.exe: $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(OBJ)/sqlite3.o: vendor/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(CC) $(SQLITE_FLAGS) -c $< -o $@

$(OBJ)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ)/cairn_res.o: res/cairn.rc res/cairn.manifest res/cairn.ico
	@mkdir -p $(dir $@)
	$(WINDRES) -O coff -Ires $< -o $@

-include $(OBJS:.o=.d)

# ---- native core tests (host compiler) ----
HOSTCXX ?= g++
HOSTCC  ?= gcc
NATIVE  := $(BUILD)/native

$(NATIVE)/sqlite3.o: vendor/sqlite/sqlite3.c
	@mkdir -p $(NATIVE)
	$(HOSTCC) $(SQLITE_FLAGS) -c $< -o $@

$(NATIVE)/core_test: tests/core_test.cpp $(CORE_SRC) $(NATIVE)/sqlite3.o
	$(HOSTCXX) -std=c++17 -O1 -g -Wall -Wextra -Isrc/core -Ivendor/sqlite $^ -o $@ -lpthread -ldl

test: $(NATIVE)/core_test
	./$(NATIVE)/core_test

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
