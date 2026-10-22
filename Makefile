NDK ?= C:/Users/Muffin_Joe/AppData/Local/Android/Sdk/ndk/28.2.13676358
TC   := $(NDK)/toolchains/llvm/prebuilt/windows-x86_64/bin
CC   := $(TC)/aarch64-linux-android28-clang.cmd
STRIP := $(TC)/llvm-strip.exe

CFLAGS := -static -O2 -Wall -Wextra
LDLIBS := -llog

all: penbridge

penbridge: penbridge.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
	$(STRIP) $@

clean:
	rm -f penbridge

.PHONY: all clean
