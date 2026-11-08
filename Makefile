CC = gcc
WINDRES ?= windres
CFLAGS = -Wall -Wextra -O2 -std=gnu11
LDFLAGS = -mwindows -lgdi32 -luser32 -lshell32 -ladvapi32 -lwtsapi32 -lwinmm -lole32 -loleaut32 -luuid -lcomdlg32 -lwininet
SRCS = main.c config.c input.c hint.c tray.c uia.c snippet.c update.c shutdown.c touchpad.c zoom.c
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

# The UIAccess build: the same program with uiAccess="true" in its manifest,
# so Windows lets the hint overlay above the Start menu, Search, the
# notification centre and notification banners (README, "Hints on the Start
# menu and notifications"). Windows runs it only signed and installed under
# Program Files: tools\uiaccess_setup.ps1 does both.
uiaccess: vim-uiaccess.exe

vim-uiaccess.exe: $(OBJS) vim.res.uiaccess.o
	$(CC) $(CFLAGS) -o $@ $(OBJS) vim.res.uiaccess.o $(LDFLAGS)

vim.res.uiaccess.o: vim.rc vim.uiaccess.manifest vim.ico version.h
	$(WINDRES) -I. -DUIACCESS -O coff -i vim.rc -o $@

# Regenerates the committed vim.ico. CI builds on Linux and uses the file as is.
icon: tools/make_icon.exe
	tools/make_icon.exe vim.ico

tools/make_icon.exe: tools/make_icon.c
	$(CC) $(CFLAGS) -o $@ tools/make_icon.c -lole32

# Console tool that lists what the element scan finds on the monitor under the cursor
probe: tools/uia_probe.exe

tools/uia_probe.exe: tools/uia_probe.c uia.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/uia_probe.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lcomdlg32

# Console tool that times how the front Chrome window reports a scroll and
# answers the hit tests of the settle wait (it scrolls the page one notch and back)
flushprobe: tools/flush_probe.exe

tools/flush_probe.exe: tools/flush_probe.c uia.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/flush_probe.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lcomdlg32 -lwinmm

# Console tool that watches a Chromium page load: uia.c's page_busy and the
# content events hint.c's page watch takes (load_probe.mjs drives it), and
# vim_post, which opens or closes the running vim mouse's hints by message
loadprobe: tools/load_probe.exe tools/vim_post.exe

tools/load_probe.exe: tools/load_probe.c uia.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/load_probe.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lcomdlg32

tools/vim_post.exe: tools/vim_post.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/vim_post.c -luser32

# Console tool that compares Chrome's DOM with what the scan finds (hint_check.mjs)
check: tools/hint_check.exe

tools/hint_check.exe: tools/hint_check.c uia.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/hint_check.c -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lcomdlg32

# Console check of the scheduled shutdown's timing with a fake clock; it never
# shuts down. Runs on Windows, or under Wine.
shutdown-test: tools/shutdown_test.exe

tools/shutdown_test.exe: tools/shutdown_test.c shutdown.c config.c common.h
	$(CC) $(CFLAGS) -I. -o $@ tools/shutdown_test.c -luser32 -lgdi32 -ladvapi32

clean:
	-@del /Q $(TARGET) 2>nul || true
	-@del /Q *.o 2>nul || true
	-@del /Q vim-uiaccess.exe 2>nul || true
	-@del /Q tools\uia_probe.exe 2>nul || true
	-@del /Q tools\hint_check.exe 2>nul || true
	-@del /Q tools\flush_probe.exe 2>nul || true
	-@del /Q tools\load_probe.exe 2>nul || true
	-@del /Q tools\vim_post.exe 2>nul || true
	-@del /Q tools\make_icon.exe 2>nul || true
	-@del /Q tools\shutdown_test.exe 2>nul || true

.PHONY: all clean probe check icon shutdown-test flushprobe loadprobe uiaccess
