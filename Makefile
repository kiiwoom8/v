CC = gcc
WINDRES ?= windres
CFLAGS = -Wall -Wextra -O2 -std=gnu11
LDFLAGS = -mwindows -lgdi32 -luser32 -lshell32 -ladvapi32 -lwtsapi32 -lwinmm -lole32 -loleaut32 -luuid -lcomdlg32 -lwininet
SRCS = main.c config.c input.c hint.c tray.c uia.c snippet.c update.c
OBJS = $(SRCS:.c=.o)
RESOBJ = vim.res.o
TARGET = vim.exe

all: $(TARGET)

$(TARGET): $(OBJS) $(RESOBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(RESOBJ) $(LDFLAGS)

%.o: %.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

tray.o update.o: version.h

vim.res.o: vim.rc vim.manifest vim.ico version.h
	$(WINDRES) -I. -O coff -i vim.rc -o $@

# Regenerates the committed vim.ico. CI builds on Linux and uses the file as is.
icon: tools/make_icon.exe
	tools/make_icon.exe vim.ico

tools/make_icon.exe: tools/make_icon.c
	$(CC) $(CFLAGS) -o $@ tools/make_icon.c -lole32

# Console tool that lists what the element scan finds on the monitor under the cursor
probe: tools/uia_probe.exe

tools/uia_probe.exe: tools/uia_probe.c uia.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/uia_probe.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lcomdlg32

# Console tool that compares Chrome's DOM with what the scan finds (hint_check.mjs)
check: tools/hint_check.exe

tools/hint_check.exe: tools/hint_check.c uia.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/hint_check.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lcomdlg32

clean:
	-@del /Q $(TARGET) 2>nul || true
	-@del /Q *.o 2>nul || true
	-@del /Q tools\uia_probe.exe 2>nul || true
	-@del /Q tools\hint_check.exe 2>nul || true
	-@del /Q tools\make_icon.exe 2>nul || true

.PHONY: all clean probe check icon
