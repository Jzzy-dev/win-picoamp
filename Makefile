# PicoAmp for Windows
#
#   make                 PicoAmp.exe (cross: llvm-mingw or mingw-w64; or MSYS2 on Windows)
#   make test            the test build for Linux (no window, null sound) and its checks
#
# CROSS: the compiler prefix, e.g. CROSS=x86_64-w64-mingw32- (default)

CROSS    ?= x86_64-w64-mingw32-
WINCC    ?= $(CROSS)gcc
WINDRES  ?= $(CROSS)windres
HOSTCC   ?= cc
VERSION  ?= 0.1.0

COMMON   = src/gfx.c src/ui.c src/fullscreen.c src/clock.c src/weather.c src/wxsvc.c src/news.c src/newsfeed.c src/audio.c src/viz.c src/eq.c src/stream.c src/net.c \
           src/library.c src/radiodir.c src/json.c src/config.c src/tags.c src/aacdec.c src/sys.c src/util.c src/png.c src/script.c src/miniaudio_impl.c
HEADERS  = $(wildcard src/*.h)
CFLAGS   = -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -Ithird_party/miniaudio -DPICOAMP_VERSION=\"$(VERSION)\"
# miniaudio's own warnings are not ours
MA_FLAGS = -w

all: build/PicoAmp.exe

build:
	mkdir -p build

build/%.win.o: src/%.c $(HEADERS) | build
	$(WINCC) $(CFLAGS) -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -c $< -o $@

build/miniaudio_impl.win.o: src/miniaudio_impl.c | build
	$(WINCC) -O2 $(MA_FLAGS) -Ithird_party/miniaudio -c $< -o $@

build/picoamp.res.o: res/picoamp.rc res/picoamp.ico res/picoamp.manifest | build
	$(WINDRES) -DPICOAMP_VERSION=\\\"$(VERSION)\\\" res/picoamp.rc -O coff -o $@

WINOBJ = $(patsubst src/%.c,build/%.win.o,$(COMMON) src/main_win32.c) build/picoamp.res.o

build/PicoAmp.exe: $(WINOBJ)
	$(WINCC) -o $@ $(WINOBJ) -mwindows -municode -static \
	    -lwinhttp -lws2_32 -lgdi32 -luser32 -lshell32 -lole32 -lwinmm -ldwmapi -lshlwapi -lmfplat -lmfuuid -luuid

# ---- the test build (Linux) ----
build/%.host.o: src/%.c $(HEADERS) | build
	$(HOSTCC) $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

build/miniaudio_impl.host.o: src/miniaudio_impl.c | build
	$(HOSTCC) -O2 $(MA_FLAGS) -D_GNU_SOURCE -Ithird_party/miniaudio -c $< -o $@

HOSTOBJ = $(patsubst src/%.c,build/%.host.o,$(COMMON) src/main_host.c)

build/picoamp_test: $(HOSTOBJ)
	$(HOSTCC) -o $@ $(HOSTOBJ) -lpthread -lm -ldl

test: build/picoamp_test
	sh tests/run.sh

clean:
	rm -rf build

.PHONY: all test clean
